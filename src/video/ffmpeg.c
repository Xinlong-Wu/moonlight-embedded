/*
 * This file is part of Moonlight Embedded.
 *
 * Based on Moonlight Pc implementation
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "ffmpeg.h"

#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include <Limelight.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

// General decoder and renderer state
static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;

static int dec_frames_cnt;
static int current_frame, next_frame;
static bool packet_pending;
static uint64_t inspect_calls, inspect_time_us, api_send_calls, send_time_us, send_max_us;
static uint64_t receive_calls, receive_time_us, receive_max_us;

// Parse compressed TH1520 input independently of the OMX component. The vendor
// wrapper may return/reallocate output buffers as soon as it sees a changed SPS,
// so a post-decode dimension check is too late to protect displayed frames.
static AVCodecParserContext* th1520_parser;
static AVCodecContext* th1520_parser_ctx;
static enum AVCodecID th1520_parser_codec = AV_CODEC_ID_NONE;
static int th1520_expected_width, th1520_expected_height;
static int th1520_coded_width, th1520_coded_height;
static int th1520_parser_error;
static uint64_t filler_nal_count, filler_byte_count, filler_noop_count;

enum decoders ffmpeg_decoder;

#define BYTES_PER_PIXEL 4

static void log_av_error(const char* operation, int err) {
  char errorstring[512];
  av_strerror(err, errorstring, sizeof(errorstring));
  fprintf(stderr, "%s - %d/%s\n", operation, err, errorstring);
}

static int th1520_reset_parser(void) {
  if (th1520_parser) av_parser_close(th1520_parser);
  th1520_parser = NULL;
  avcodec_free_context(&th1520_parser_ctx);
  th1520_parser = av_parser_init(th1520_parser_codec);
  th1520_parser_ctx = avcodec_alloc_context3(NULL);
  if (!th1520_parser || !th1520_parser_ctx) {
    fprintf(stderr, "TH1520: cannot allocate independent H.264/HEVC size parser\n");
    return AVERROR(ENOMEM);
  }
  th1520_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
  th1520_parser_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  th1520_parser_ctx->codec_id = th1520_parser_codec;
  return 0;
}

// A new parameter set must not be validated using a stale size from the old
// parser state: FFmpeg parsers can log a malformed SPS but return success.
static bool th1520_has_parameter_set(const uint8_t* data, int size) {
  for (int i = 0; i + 3 < size; i++) {
    if (data[i] || data[i + 1]) continue;
    int header;
    if (data[i + 2] == 1) header = i + 3;
    else if (data[i + 2] == 0 && i + 4 < size && data[i + 3] == 1) header = i + 4;
    else continue;
    if (th1520_parser_codec == AV_CODEC_ID_H264) {
      unsigned type = data[header] & 0x1f;
      if (type == 7 || type == 8) return true;
    } else {
      unsigned type = (data[header] >> 1) & 0x3f;
      if (type == 32 || type == 33 || type == 34) return true;
    }
    i = header - 1;
  }
  return false;
}

static int th1520_validate_size(const uint8_t* data, int size) {
  if (th1520_parser_error) return th1520_parser_error;
  if (!th1520_parser || !th1520_parser_ctx) return AVERROR(EINVAL);
  // Moonlight supplies complete parameter sets with IDR access units. Reject
  // partial/malformed configuration rather than trusting dimensions retained
  // from an earlier SPS. This deliberately favors safety over partial-config
  // streams that require combining old and new parameter sets.
  if (th1520_has_parameter_set(data, size)) {
    int err = th1520_reset_parser();
    if (err < 0) return th1520_parser_error = err;
  }
  uint8_t* output = NULL;
  int output_size = 0;
  int consumed = av_parser_parse2(th1520_parser, th1520_parser_ctx, &output,
                                  &output_size, data, size, AV_NOPTS_VALUE,
                                  AV_NOPTS_VALUE, 0);
  if (consumed < 0 || consumed != size || output_size != size ||
      th1520_parser->width <= 0 || th1520_parser->height <= 0 ||
      th1520_parser->coded_width < th1520_parser->width ||
      th1520_parser->coded_height < th1520_parser->height) {
    fprintf(stderr, "TH1520: cannot validate complete AU dimensions; refusing compressed input to fixed-size OMX component\n");
    return th1520_parser_error = AVERROR_INVALIDDATA;
  }
  // Compare visible dimensions, not macroblock-aligned coded dimensions:
  // 1366x768 is valid even when H.264 reports coded_width == 1376.
  if (th1520_parser->width != th1520_expected_width ||
      th1520_parser->height != th1520_expected_height ||
      (th1520_coded_width && th1520_parser->coded_width != th1520_coded_width) ||
      (th1520_coded_height && th1520_parser->coded_height != th1520_coded_height)) {
    fprintf(stderr, "TH1520: fixed-size OMX setup %dx%d does not support AU %dx%d (coded %dx%d); resolution changes are unsupported\n",
            th1520_expected_width, th1520_expected_height,
            th1520_parser->width, th1520_parser->height,
            th1520_parser->coded_width, th1520_parser->coded_height);
    return th1520_parser_error = AVERROR_INPUT_CHANGED;
  }
  th1520_coded_width = th1520_parser->coded_width;
  th1520_coded_height = th1520_parser->coded_height;
  return 0;
}

// Return a start-code prefix, excluding preceding Annex-B leading/trailing zeros.
static int h264_start_code(const uint8_t* data, int size, int from, int* prefix) {
  for (int i = from; i + 2 < size; i++) {
    if (data[i] || data[i + 1]) continue;
    if (data[i + 2] == 1) { *prefix = 3; return i; }
    if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
      *prefix = 4; return i;
    }
  }
  *prefix = 0;
  return size;
}

// H.264 7.3.2.7 filler_data_rbsp: filler_ff_byte* followed by the byte-aligned
// rbsp_trailing_bits (0x80). Remaining zeros belong to Annex-B framing. A type12
// header alone is not sufficient: malformed/escaped/arbitrary payloads and any
// nonzero nal_ref_idc are retained rather than silently discarded.
static bool h264_valid_filler(const uint8_t* data, int header, int end) {
  if (data[header] != 12) return false;
  int cursor = header + 1;
  while (cursor < end && data[cursor] == 0xff) cursor++;
  if (cursor == end || data[cursor++] != 0x80) return false;
  while (cursor < end && data[cursor] == 0) cursor++;
  return cursor == end;
}

// Normalize only a complete H.264 Annex-B AU owned by this sender. Legal
// filler_data NALs carry no decoded picture information but trigger a very slow
// Hantro software path for static CBR streams; remove those NALs, not payloads
// merely resembling them. Validate the full framing before in-place compaction.
// All retained NAL bytes/EPBs and leading zeros are copied verbatim. Finally,
// shorten only an excess all-zero AU tail to two zeros: these may belong to a
// CABAC cabac_zero_word or terminal 00 00 03 00, so never trim to a dangling EPB.
// An empty result means an all-legal-filler AU and must be consumed without
// sending a zero-size AVPacket (which would signal EOS). This is not a full
// malformed-bitstream/RBSP validator.
static int th1520_normalize_h264_packet(AVPacket* packet) {
  if (!packet || !packet->data || packet->size <= 0) return AVERROR_INVALIDDATA;
  uint8_t* data = packet->data;
  int original_size = packet->size;
  int prefix, first = h264_start_code(data, original_size, 0, &prefix);
  if (first == original_size) return AVERROR_INVALIDDATA;
  for (int i = 0; i < first; i++)
    if (data[i] != 0) return AVERROR_INVALIDDATA;

  uint64_t removed_nals = 0, removed_bytes = 0;
  int retained_nals = 0, position = first;
  // First pass does not mutate the packet or counters, even if a later NAL is
  // malformed. An embedded valid start code is a NAL boundary by definition.
  while (position < original_size) {
    int header = position + prefix;
    if (header >= original_size || (data[header] & 0x80) ||
        (data[header] & 0x1f) == 0 || (data[header] & 0x1f) >= 24)
      return AVERROR_INVALIDDATA;
    int next_prefix;
    int next = h264_start_code(data, original_size, header + 1, &next_prefix);
    if (h264_valid_filler(data, header, next)) {
      removed_nals++;
      removed_bytes += next - position;
    } else {
      retained_nals++;
      int end = next;
      while (end > header + 1 && data[end - 1] == 0) end--;
      if (end == next && end - header >= 4 && data[end - 1] == 3 &&
          data[end - 2] == 0 && data[end - 3] == 0)
        return AVERROR_INVALIDDATA;
    }
    position = next;
    prefix = next_prefix;
  }

  if (!retained_nals) {
    packet->size = 0;
    memset(data, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  } else if (removed_nals) {
    int written = first;
    position = h264_start_code(data, original_size, 0, &prefix);
    while (position < original_size) {
      int header = position + prefix;
      int next_prefix;
      int next = h264_start_code(data, original_size, header + 1, &next_prefix);
      if (!h264_valid_filler(data, header, next)) {
        memmove(data + written, data + position, next - position);
        written += next - position;
      }
      position = next;
      prefix = next_prefix;
    }
    packet->size = written;
  }

  if (packet->size > 0) {
    int nonzero_end = packet->size;
    while (nonzero_end > 0 && data[nonzero_end - 1] == 0) nonzero_end--;
    if (packet->size - nonzero_end > 2) packet->size = nonzero_end + 2;
    // A shorter logical packet still requires zero padding at its new end.
    memset(data + packet->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  }
  filler_nal_count += removed_nals;
  filler_byte_count += removed_bytes;
  return 0;
}

// This function must be called before
// any other decoding functions
int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count) {
  // Also clean up a partially initialized or previously used decoder.
  ffmpeg_destroy();

  // Keep vendor OMX initialization and device errors visible. Other backends
  // retain their historical quiet logging setting.
  av_log_set_level((perf_lvl & TH1520_ACCELERATION) ? AV_LOG_INFO : AV_LOG_QUIET);

  // Initialize the avcodec library and register codecs
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
  avcodec_register_all();
#endif

  if (buffer_count <= 0 || width <= 0 || height <= 0) {
    fprintf(stderr, "Invalid decoder dimensions or frame buffer count\n");
    return -1;
  }

  ffmpeg_decoder = (perf_lvl & TH1520_ACCELERATION) ? TH1520_OMX :
                   (perf_lvl & VAAPI_ACCELERATION) ? VAAPI : SOFTWARE;

  if (ffmpeg_decoder == TH1520_OMX && videoFormat != VIDEO_FORMAT_H264 && videoFormat != VIDEO_FORMAT_H265) {
    fprintf(stderr, "TH1520 OMX decoding only supports 8-bit 4:2:0 H.264 and HEVC\n");
    return -1;
  }

  if (ffmpeg_decoder == TH1520_OMX) {
    th1520_expected_width = width;
    th1520_expected_height = height;
    th1520_parser_codec = videoFormat == VIDEO_FORMAT_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    if (th1520_reset_parser() < 0) goto fail;
  }

  pkt = av_packet_alloc();
  if (pkt == NULL) {
    printf("Couldn't allocate packet\n");
    goto fail;
  }

  int max_tries = ffmpeg_decoder == TH1520_OMX ? 1 : 6;
  for (int try = 0; try < max_tries; try++) {
    // Never let a failed attempt leave a stale decoder selection behind.
    decoder = NULL;

    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
      if (ffmpeg_decoder == TH1520_OMX) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("h264_omx");
      } else {
        if (ffmpeg_decoder == SOFTWARE) {
          if (try == 0) decoder = avcodec_find_decoder_by_name("h264_nvv4l2"); // Tegra
          if (try == 1) decoder = avcodec_find_decoder_by_name("h264_nvmpi"); // Tegra
          if (try == 2) decoder = avcodec_find_decoder_by_name("h264_omx"); // VisionFive
          if (try == 3) decoder = avcodec_find_decoder_by_name("h264_v4l2m2m"); // Stateful V4L2
        }
        if (try == 4) decoder = avcodec_find_decoder_by_name("h264"); // Software and hwaccel
      }
    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
      if (ffmpeg_decoder == TH1520_OMX) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("hevc_omx");
      } else {
        if (ffmpeg_decoder == SOFTWARE) {
          if (try == 0) decoder = avcodec_find_decoder_by_name("hevc_nvv4l2"); // Tegra
          if (try == 1) decoder = avcodec_find_decoder_by_name("hevc_nvmpi"); // Tegra
          if (try == 2) decoder = avcodec_find_decoder_by_name("hevc_omx"); // VisionFive
          if (try == 3) decoder = avcodec_find_decoder_by_name("hevc_v4l2m2m"); // Stateful V4L2
        }
        if (try == 4) decoder = avcodec_find_decoder_by_name("hevc"); // Software and hwaccel
      }
    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
      if (ffmpeg_decoder != TH1520_OMX) {
        if (ffmpeg_decoder == SOFTWARE) {
          if (try == 0) decoder = avcodec_find_decoder_by_name("libdav1d");
        }
        if (try == 1) decoder = avcodec_find_decoder_by_name("av1"); // Hwaccel
      }
    } else {
      printf("Video format not supported\n");
      goto fail;
    }

    // Skip this decoder if it isn't compiled into FFmpeg
    if (!decoder) {
      continue;
    }

    // SDL and the generic EGL renderer consume planar YUV420. Do not select
    // a vendor wrapper that explicitly advertises only NV12/DRM PRIME there.
    if (ffmpeg_decoder == SOFTWARE && decoder->pix_fmts != NULL) {
      const enum AVPixelFormat* format = decoder->pix_fmts;
      while (*format != AV_PIX_FMT_NONE && *format != AV_PIX_FMT_YUV420P) format++;
      if (*format == AV_PIX_FMT_NONE) { decoder = NULL; continue; }
    }

    decoder_ctx = avcodec_alloc_context3(decoder);
    if (decoder_ctx == NULL) {
      printf("Couldn't allocate context\n");
      goto fail;
    }

    // Use low delay decoding
    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Allow display of corrupt frames and frames missing references
    decoder_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    decoder_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;

    // Report decoding errors to allow us to request a key frame
    decoder_ctx->err_recognition = AV_EF_EXPLODE;

    if (ffmpeg_decoder == TH1520_OMX) {
      decoder_ctx->thread_count = 1;
      // Moonlight presentation timestamps and OMX nTimeStamp are microseconds.
      decoder_ctx->time_base = (AVRational) { 1, AV_TIME_BASE };
      decoder_ctx->pkt_timebase = decoder_ctx->time_base;
    } else if (perf_lvl & SLICE_THREADING) {
      decoder_ctx->thread_type = FF_THREAD_SLICE;
      decoder_ctx->thread_count = thread_count;
    } else {
      decoder_ctx->thread_count = 1;
    }

    decoder_ctx->width = width;
    decoder_ctx->height = height;
    decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    AVDictionary* decoder_options = NULL;
    if (ffmpeg_decoder == TH1520_OMX) {
      bool nv12 = (perf_lvl & TH1520_NV12) != 0;
      // Upstream and other vendors also expose codecs named *_omx. Only the
      // RevyOS wrapper has the output-mode option required by this backend.
      if (decoder_ctx->priv_data == NULL ||
          av_opt_find(decoder_ctx->priv_data, "drm_prime", NULL,
                      AV_OPT_FLAG_DECODING_PARAM, 0) == NULL) {
        fprintf(stderr, "TH1520 requires the RevyOS %s decoder with the drm_prime option\n", decoder->name);
        goto fail;
      }
      if (!nv12 && av_opt_find(decoder_ctx->priv_data, "th1520_drm_lifetime_fixed", "th1520_fixes",
                              AV_OPT_FLAG_DECODING_PARAM, 0) == NULL) {
        const char* mode = getenv("MOONLIGHT_TH1520_OUTPUT");
        if (mode && strcmp(mode, "drm") == 0) {
          fprintf(stderr, "TH1520: WARNING: forced DRM output on an unverified FFmpeg; RevyOS 5.1.4 has per-frame lifetime leaks. See docs/TH1520.md.\n");
        } else {
          fprintf(stderr, "TH1520: selecting hardware NV12 output: FFmpeg DRM lifetime fix is not detected.\n");
          nv12 = true;
          perf_lvl |= TH1520_NV12;
        }
      }
      decoder_ctx->pix_fmt = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_DRM_PRIME;
      if (av_dict_set(&decoder_options, "drm_prime", nv12 ? "0" : "1", 0) < 0) {
        fprintf(stderr, "Couldn't allocate TH1520 decoder options\n");
        av_dict_free(&decoder_options);
        goto fail;
      }
    }

    int err = avcodec_open2(decoder_ctx, decoder, &decoder_options);
    bool unused_output_mode = ffmpeg_decoder == TH1520_OMX &&
                              av_dict_get(decoder_options, "drm_prime", NULL, 0) != NULL;
    av_dict_free(&decoder_options);
    if (err < 0) {
      fprintf(stderr, "Couldn't open codec: %s\n", decoder->name);
      log_av_error("Codec initialization failed", err);
      avcodec_free_context(&decoder_ctx);
      decoder = NULL;
      continue;
    }
    if (unused_output_mode) {
      fprintf(stderr, "TH1520 decoder %s did not consume the drm_prime option\n", decoder->name);
      goto fail;
    }

    // The selected context and decoder must remain paired.
    break;
  }

  if (decoder == NULL || decoder_ctx == NULL) {
    printf("Couldn't find or open a supported decoder\n");
    goto fail;
  }

  dec_frames_cnt = buffer_count;
  dec_frames = calloc(buffer_count, sizeof(AVFrame*));
  if (dec_frames == NULL) {
    fprintf(stderr, "Couldn't allocate frames\n");
    goto fail;
  }

  for (int i = 0; i < buffer_count; i++) {
    dec_frames[i] = av_frame_alloc();
    if (dec_frames[i] == NULL) {
      fprintf(stderr, "Couldn't allocate frame\n");
      goto fail;
    }
  }

  #ifdef HAVE_VAAPI
  if (ffmpeg_decoder == VAAPI)
    vaapi_init(decoder_ctx);
  #endif

  printf("Using FFmpeg decoder: %s\n", decoder->name);
  if (ffmpeg_decoder == TH1520_OMX)
    printf("TH1520 OMX output: %s\n", (perf_lvl & TH1520_NV12) ? "NV12" : "DRM PRIME");
  return 0;

fail:
  ffmpeg_destroy();
  return -1;
}

// This function must be called after
// decoding is finished
void ffmpeg_destroy(void) {
  if (ffmpeg_decoder == TH1520_OMX && (api_send_calls || receive_calls))
    printf("TH1520 API timing: inspect_avg_ms=%.3f send_avg_ms=%.3f send_max_ms=%.3f receive_avg_ms=%.3f receive_max_ms=%.3f\n",
           inspect_calls ? inspect_time_us / (1000.0 * inspect_calls) : 0.0,
           api_send_calls ? send_time_us / (1000.0 * api_send_calls) : 0.0, send_max_us / 1000.0,
           receive_calls ? receive_time_us / (1000.0 * receive_calls) : 0.0, receive_max_us / 1000.0);
  if (ffmpeg_decoder == TH1520_OMX && (inspect_calls || filler_nal_count))
    printf("TH1520 input: filler_nals_removed=%" PRIu64 " filler_bytes_removed=%" PRIu64 " filler_only_noops=%" PRIu64 "\n",
           filler_nal_count, filler_byte_count, filler_noop_count);
  filler_nal_count = filler_byte_count = filler_noop_count = 0;
  inspect_calls = inspect_time_us = api_send_calls = send_time_us = send_max_us = 0;
  receive_calls = receive_time_us = receive_max_us = 0;
  av_packet_free(&pkt);
  packet_pending = false;
  if (th1520_parser) av_parser_close(th1520_parser);
  th1520_parser = NULL;
  avcodec_free_context(&th1520_parser_ctx);
  th1520_parser_codec = AV_CODEC_ID_NONE;
  th1520_expected_width = th1520_expected_height = 0;
  th1520_coded_width = th1520_coded_height = 0;
  th1520_parser_error = 0;

  // OMX frames own output buffers and decoder references. Return every local
  // frame before closing the context; the renderer must first release any
  // clones/EGL images it owns, too.
  if (dec_frames) {
    for (int i = 0; i < dec_frames_cnt; i++)
      av_frame_free(&dec_frames[i]);
    free(dec_frames);
    dec_frames = NULL;
  }
  avcodec_free_context(&decoder_ctx);
  decoder = NULL;
  dec_frames_cnt = 0;
  current_frame = 0;
  next_frame = 0;
}

// A few legacy wrappers don't advertise their formats until the first frame.
// The generic SDL/EGL renderers require three planar 8-bit channels. Normalize
// semiplanar output here rather than handing them a NULL chroma-plane pointer.
// The TH1520 path bypasses this CPU conversion entirely.
static int normalize_software_frame(AVFrame* frame) {
  if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P)
    return 0;
  if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_NV21) {
    fprintf(stderr, "Generic renderer cannot display FFmpeg pixel format %d\n", frame->format);
    return AVERROR(ENOSYS);
  }
  int chroma_width = (frame->width + 1) / 2, chroma_height = (frame->height + 1) / 2;
  if (!frame->data[0] || !frame->data[1] ||
      llabs((long long)frame->linesize[0]) < frame->width ||
      llabs((long long)frame->linesize[1]) < chroma_width * 2)
    return AVERROR_INVALIDDATA;
  AVFrame* planar = av_frame_alloc();
  if (!planar) return AVERROR(ENOMEM);
  planar->format = AV_PIX_FMT_YUV420P;
  planar->width = frame->width; planar->height = frame->height;
  int result = av_frame_get_buffer(planar, 32);
  if (result >= 0) result = av_frame_copy_props(planar, frame);
  if (result >= 0) {
    for (int y = 0; y < frame->height; y++)
      memcpy(planar->data[0] + (ptrdiff_t)y * planar->linesize[0],
             frame->data[0] + (ptrdiff_t)y * frame->linesize[0], frame->width);
    int u = frame->format == AV_PIX_FMT_NV12 ? 0 : 1;
    for (int y = 0; y < chroma_height; y++) {
      const unsigned char* row = frame->data[1] + (ptrdiff_t)y * frame->linesize[1];
      for (int x = 0; x < chroma_width; x++) {
        planar->data[1][(ptrdiff_t)y * planar->linesize[1] + x] = row[2 * x + u];
        planar->data[2][(ptrdiff_t)y * planar->linesize[2] + x] = row[2 * x + 1 - u];
      }
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, planar);
  }
  av_frame_free(&planar);
  return result;
}

static int receive_frame(bool native_frame, AVFrame** out_frame) {
  *out_frame = NULL;

  if (decoder_ctx == NULL || dec_frames == NULL)
    return AVERROR(EINVAL);

  int64_t begin = av_gettime_relative();
  int err = avcodec_receive_frame(decoder_ctx, dec_frames[next_frame]);
  uint64_t elapsed = av_gettime_relative() - begin;
  receive_calls++; receive_time_us += elapsed;
  if (elapsed > receive_max_us) receive_max_us = elapsed;
  if (err == 0 && ffmpeg_decoder == SOFTWARE) {
    err = normalize_software_frame(dec_frames[next_frame]);
    if (err < 0) av_frame_unref(dec_frames[next_frame]);
  }
  if (err == 0) {
    current_frame = next_frame;
    next_frame = (current_frame + 1) % dec_frames_cnt;

    if (ffmpeg_decoder == SOFTWARE || native_frame)
      *out_frame = dec_frames[current_frame];
  } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
    log_av_error("Receive failed", err);
  }
  return err;
}

AVFrame* ffmpeg_get_frame(bool native_frame) {
  AVFrame* frame = NULL;
  receive_frame(native_frame, &frame);
  return frame;
}

static int send_packet_ex(unsigned char* indata, int inlen, bool key_frame, int64_t pts_us) {
  if (decoder_ctx == NULL || pkt == NULL || inlen < 0 ||
      (inlen > 0 && indata == NULL) || (inlen == 0 && indata != NULL))
    return AVERROR(EINVAL);

  if (packet_pending && (indata != NULL || inlen != 0)) {
    // The pending packet must be drained before accepting new input.
    return AVERROR(EAGAIN);
  }

  bool submitting = packet_pending || indata != NULL;
  if (!packet_pending && submitting) {
    // Own the packet until FFmpeg accepts it. In particular, do not lose the
    // caller's buffer when avcodec_send_packet() asks us to receive first.
    av_packet_unref(pkt);
    int err = av_new_packet(pkt, inlen);
    if (err < 0) {
      log_av_error("Couldn't allocate encoded packet", err);
      return err;
    }
    memcpy(pkt->data, indata, inlen);
    pkt->flags = key_frame ? AV_PKT_FLAG_KEY : 0;
    pkt->pts = pts_us;
    // No independent decode timestamp is provided by this API.
    pkt->dts = AV_NOPTS_VALUE;
    // Normalize and validate the owned copy once, before OMX sees it. NULL/0
    // retries retain exactly the original bytes, flags and microsecond PTS.
    if (ffmpeg_decoder == TH1520_OMX) {
      int64_t inspect_begin = av_gettime_relative();
      if (th1520_parser_codec == AV_CODEC_ID_H264) {
        err = th1520_normalize_h264_packet(pkt);
        if (err < 0) {
          fprintf(stderr, "TH1520: invalid H.264 Annex-B access unit\n");
          av_packet_unref(pkt);
          return err;
        }
        if (pkt->size == 0) {
          // An all-filler AU has no picture to decode. Consume it as accepted,
          // but never submit a zero-size packet or convert this into EOS.
          filler_noop_count++;
          inspect_calls++;
          inspect_time_us += av_gettime_relative() - inspect_begin;
          av_packet_unref(pkt);
          return 0;
        }
      }
      err = th1520_validate_size(pkt->data, pkt->size);
      if (err < 0) {
        av_packet_unref(pkt);
        return err;
      }
      inspect_calls++;
      inspect_time_us += av_gettime_relative() - inspect_begin;
    }
    packet_pending = true;
  }

  if (!packet_pending)
    return 0;

  int64_t begin = av_gettime_relative();
  int err = avcodec_send_packet(decoder_ctx, pkt);
  uint64_t elapsed = av_gettime_relative() - begin;
  api_send_calls++; send_time_us += elapsed;
  if (elapsed > send_max_us) send_max_us = elapsed;
  if (err == AVERROR(EAGAIN))
    return err;

  packet_pending = false;
  av_packet_unref(pkt);
  if (err < 0)
    log_av_error("Decode failed", err);
  return err;
}

static int send_packet(unsigned char* indata, int inlen) {
  return send_packet_ex(indata, inlen, false, AV_NOPTS_VALUE);
}

// Legacy callers only submit once and then receive; they do not implement the
// saved-packet retry protocol. Return EAGAIN without retaining a pending packet
// here, or their next input would be rejected forever. Callers requiring lossless
// retries must use ffmpeg_decode_frame() instead. Do not mix the two APIs.
int ffmpeg_decode(unsigned char* indata, int inlen) {
  int err = send_packet(indata, inlen);
  if (err == AVERROR(EAGAIN)) {
    av_packet_unref(pkt);
    packet_pending = false;
  }
  return err;
}

int ffmpeg_decode_frame_ex(unsigned char* indata, int inlen, bool key_frame, int64_t pts_us, AVFrame** out_frame) {
  if (out_frame == NULL)
    return AVERROR(EINVAL);
  *out_frame = NULL;

  if (packet_pending && (indata != NULL || inlen != 0)) {
    // Do not mistake rejection of new input for EAGAIN on the saved packet.
    return AVERROR(EAGAIN);
  }

  bool submitting = packet_pending || indata != NULL || inlen != 0;
  int send_err = send_packet_ex(indata, inlen, key_frame, pts_us);
  if (send_err < 0 && send_err != AVERROR(EAGAIN))
    return send_err;

  int status = 0;
  if (send_err == AVERROR(EAGAIN))
    status |= FFMPEG_DECODE_PACKET_PENDING;
  else if (submitting)
    status |= FFMPEG_DECODE_PACKET_ACCEPTED;

  int receive_err = receive_frame(true, out_frame);
  if (receive_err == 0)
    status |= FFMPEG_DECODE_FRAME_AVAILABLE;
  else if (receive_err != AVERROR(EAGAIN) && receive_err != AVERROR_EOF)
    return receive_err;

  return status;
}

int ffmpeg_decode_frame(unsigned char* indata, int inlen, AVFrame** out_frame) {
  return ffmpeg_decode_frame_ex(indata, inlen, false, AV_NOPTS_VALUE, out_frame);
}
