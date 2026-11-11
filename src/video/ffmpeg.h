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

#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

// Enable multi-threaded decoding
#define SLICE_THREADING 0x4
// Uses hardware acceleration
#define VDPAU_ACCELERATION 0x40
#define VAAPI_ACCELERATION 0x80
#define TH1520_ACCELERATION 0x100
// Select NV12 instead of DRM PRIME for the TH1520 OMX decoder
#define TH1520_NV12 0x200

enum decoders {SOFTWARE, VDPAU, VAAPI, TH1520_OMX};
extern enum decoders ffmpeg_decoder;

int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count);
// TH1520 input is limited to the setup's visible width/height. An independent
// public H.264/HEVC parser checks each new complete Annex-B AU before OMX sees
// it; coded dimensions may include padding (e.g. 1366 visible / 1376 coded).
// Unknown/malformed dimensions and partial parameter-set updates are rejected
// conservatively, as are visible or coded dimension changes. Complete IDRs
// must carry their SPS/PPS (and VPS for HEVC). This is a size guard, not a full
// malformed-bitstream validator.
// The renderer must release all cloned frames and EGL imports before destroy
// or reinitialization. Both also discard any saved input packet and size parser.
void ffmpeg_destroy(void);

int ffmpeg_draw_frame(AVFrame *pict);
// Borrowed frame; never av_frame_free() it. Clone it before retaining it across
// further receive calls, and av_frame_unref() it once its data is no longer used.
AVFrame* ffmpeg_get_frame(bool native_frame);
// Legacy one-shot send: EAGAIN is returned without retaining the packet.
// Use ffmpeg_decode_frame() for lossless retries; do not mix the two send APIs.
int ffmpeg_decode(unsigned char* indata, int inlen);

// Results for ffmpeg_decode_frame()
#define FFMPEG_DECODE_PACKET_ACCEPTED 0x1
#define FFMPEG_DECODE_FRAME_AVAILABLE 0x2
#define FFMPEG_DECODE_PACKET_PENDING  0x4

// Attempts at most one send and one receive. The input packet is copied before
// sending. If avcodec_send_packet() returns EAGAIN, the copy remains pending
// and FFMPEG_DECODE_PACKET_PENDING is returned. Retry it by calling with
// indata == NULL and inlen == 0 before submitting another packet. The caller
// may therefore drain and retry without losing or reordering packets.
// Returns a non-negative bitfield of the flags above, or a negative FFmpeg
// error. *out_frame is set when FFMPEG_DECODE_FRAME_AVAILABLE is present and is
// borrowed from the decoder's frame ring, not transferred to the caller. Clone
// it before queuing/retaining it, then immediately av_frame_unref(*out_frame)
// after cloning or synchronous use to return unused OMX buffers promptly.
// A negative error requires ffmpeg_init() to reset the decoder before new input;
// release all cloned/displayed frames before rebuilding it. NULL/0 polls only:
// it does not signal end-of-stream to the decoder.
int ffmpeg_decode_frame(unsigned char* indata, int inlen, AVFrame** out_frame);

// Extended form: the original new packet's key-frame flag and presentation
// timestamp are retained across EAGAIN. pts_us is in microseconds; use
// AV_NOPTS_VALUE when unavailable. NULL/0 retries ignore key_frame/pts_us and
// never signal EOS. The old wrapper supplies false and AV_NOPTS_VALUE.
// Only TH1520 H.264 Annex-B input has excess all-zero transport tails shortened
// to at most two zeros, preserving possible CABAC/terminal escaped-zero bytes.
// Internal NALs/EPBs, HEVC input and other backends are not rewritten.
int ffmpeg_decode_frame_ex(unsigned char* indata, int inlen, bool key_frame, int64_t pts_us, AVFrame** out_frame);
