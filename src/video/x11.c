/*
 * This file is part of Moonlight Embedded.
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * Moonlight is distributed WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See <http://www.gnu.org/licenses/> for a copy of the license.
 */

#define _POSIX_C_SOURCE 200809L
#include "video.h"
#include "egl.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#include "../input/x11.h"
#include "../loop.h"
#include "../util.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <inttypes.h>

#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
#define X11_TH1520_ACCELERATION 0x100
#define SLICES_PER_FRAME 4
#define MAX_DECODE_STEPS 64

static void* ffmpeg_buffer;
static size_t ffmpeg_buffer_size;
static Display* display;
static Window window;
static int pipefd[2] = { -1, -1 };
static int display_width, display_height;
static bool egl_initialized, input_initialized, stopping;

// A single decoded-frame mailbox bounds OMX buffer retention independently of
// the pipe capacity. Replacing a displayed frame never drops compressed input.
static AVFrame* queued_frame;
static unsigned long decoded_frames, presented_frames, replaced_frames;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
// Only submission and actual decoder reinitialization use this lock. Normal
// presentation must never wait behind a vendor OMX send/receive operation.
static pthread_mutex_t decoder_mutex = PTHREAD_MUTEX_INITIALIZER;
// Short control-state operations only: no FFmpeg, EGL or frame-unref calls.
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t submit_calls, submit_total_us, submit_max_us, queue_max_us;
static uint64_t render_calls, render_total_us, render_max_us;

#ifdef HAVE_TH1520
static bool th1520_active, force_nv12, require_drm, reset_requested, await_idr, fatal_error;
static bool last_frame_valid;
static bool trace_input;
static uint32_t last_frame_number;
static unsigned long input_gaps, input_idrs;
static int video_format, video_width, video_height;
#endif

static uint64_t monotonic_us(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

// 0 = ready, 1 = stopping, -1 = waiting for a renderer-side restart/failure.
static int submission_state(void) {
  int result = 0;
  pthread_mutex_lock(&state_mutex);
  if (stopping) result = 1;
#ifdef HAVE_TH1520
  else if (th1520_active && (fatal_error || reset_requested)) result = -1;
#endif
  pthread_mutex_unlock(&state_mutex);
  return result;
}

static void notify_renderer(void) {
  const char token = 1;
  ssize_t result;
  do { result = write(pipefd[1], &token, sizeof(token)); } while (result < 0 && errno == EINTR);
  // EAGAIN means a wakeup is already queued. No frame ownership lives in the pipe.
  if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    perror("X11: renderer notification failed");
}

static AVFrame* take_frame(void) {
  pthread_mutex_lock(&queue_mutex);
  AVFrame* frame = queued_frame;
  queued_frame = NULL;
  pthread_mutex_unlock(&queue_mutex);
  return frame;
}

static void queue_frame(AVFrame* frame, uint8_t colorspace) {
  AVFrame* clone = av_frame_clone(frame);
  if (!clone) return;
  // Parsed bitstream metadata takes precedence over the host's fallback hint.
  if (clone->colorspace == AVCOL_SPC_UNSPECIFIED) {
    switch (colorspace) {
    case COLORSPACE_REC_709: clone->colorspace = AVCOL_SPC_BT709; break;
    case COLORSPACE_REC_2020: clone->colorspace = AVCOL_SPC_BT2020_NCL; break;
    default: clone->colorspace = AVCOL_SPC_SMPTE170M; break;
    }
  }
  pthread_mutex_lock(&queue_mutex);
  AVFrame* previous = queued_frame;
  decoded_frames++;
  if (previous) replaced_frames++;
  queued_frame = clone;
  pthread_mutex_unlock(&queue_mutex);
  av_frame_free(&previous);
  notify_renderer();
}

#ifdef HAVE_TH1520
static int next_nal(const unsigned char* data, int length, int start, int* prefix) {
  for (int i = start; i + 3 < length; i++) {
    if (data[i] || data[i + 1]) continue;
    if (data[i + 2] == 1) { *prefix = 3; return i; }
    if (!data[i + 2] && data[i + 3] == 1) { *prefix = 4; return i; }
  }
  return length;
}

static void trace_h264_input(const unsigned char* data, int length, int number) {
  // Types and lengths only: no compressed picture bytes or desktop images.
  char summary[768];
  int used = snprintf(summary, sizeof(summary), "TH1520 NAL summary frame=%d bytes=%d:", number, length);
  int prefix = 0, position = next_nal(data, length, 0, &prefix);
  int count = 0;
  while (position < length && count++ < 24 && used < (int)sizeof(summary) - 40) {
    int header = position + prefix, next_prefix = 0;
    if (header >= length) break;
    int next = next_nal(data, length, header + 1, &next_prefix);
    used += snprintf(summary + used, sizeof(summary) - used, " type=%u/len=%d", data[header] & 31, next - position);
    position = next; prefix = next_prefix;
  }
  fprintf(stderr, "%s\n", summary);
}

static void request_decoder_reset(bool fatal) {
  pthread_mutex_lock(&state_mutex);
  if (fatal) fatal_error = true;
  else reset_requested = true;
  pthread_mutex_unlock(&state_mutex);
  notify_renderer();
}

// Rendering thread only. The restart flag prevents new submissions while an
// existing vendor call finishes. Retire GPU-held output before waiting for
// decoder exclusivity, then drop the final mailbox frame with that exclusivity.
static int reset_decoder(void) {
  if (egl_release_frame() < 0) {
    fprintf(stderr, "TH1520: cannot retire GPU frame; refusing decoder restart\n");
    request_decoder_reset(true);
    return -1;
  }
  pthread_mutex_lock(&decoder_mutex);
  pthread_mutex_lock(&state_mutex);
  bool stop = stopping;
  pthread_mutex_unlock(&state_mutex);
  if (stop) { pthread_mutex_unlock(&decoder_mutex); return -1; }
  AVFrame* queued = take_frame();
  av_frame_free(&queued);
  int result = ffmpeg_init(video_format, video_width, video_height,
                           TH1520_ACCELERATION | (force_nv12 ? TH1520_NV12 : 0), 2, 1);
  last_frame_valid = false;
  pthread_mutex_lock(&state_mutex);
  reset_requested = false;
  await_idr = true;
  if (result < 0) fatal_error = true;
  pthread_mutex_unlock(&state_mutex);
  pthread_mutex_unlock(&decoder_mutex);
  if (result < 0) fprintf(stderr, "TH1520: decoder restart failed (no software fallback)\n");
  return result;
}
#endif

static int frame_handle(int fd) {
  char tokens[128];
  while (read(fd, tokens, sizeof(tokens)) > 0) {}
#ifdef HAVE_TH1520
  pthread_mutex_lock(&state_mutex);
  bool fatal = th1520_active && fatal_error;
  bool reset = th1520_active && reset_requested;
  bool stop = stopping;
  pthread_mutex_unlock(&state_mutex);
  if (fatal || stop) return LOOP_RETURN;
  if (reset) return reset_decoder() < 0 ? LOOP_RETURN : LOOP_OK;
#endif
  AVFrame* frame = take_frame();
  if (!frame) return LOOP_OK;
#ifdef HAVE_TH1520
  int result = 0;
  if (th1520_active) {
    uint64_t begin = monotonic_us();
    if (frame->format == AV_PIX_FMT_DRM_PRIME) result = egl_draw_drm_prime(frame);
    else if (frame->format == AV_PIX_FMT_NV12) result = egl_draw_nv12(frame);
    else result = -1;
    uint64_t elapsed = monotonic_us() - begin;
    render_calls++; render_total_us += elapsed;
    if (elapsed > render_max_us) render_max_us = elapsed;
    if (elapsed >= 50000)
      fprintf(stderr, "TH1520 slow render: pts_us=%" PRId64 " elapsed_ms=%.3f\n", frame->pts, elapsed / 1000.0);
    if (result == 0) presented_frames++;
    av_frame_free(&frame);
    if (result < 0) {
      if (!force_nv12 && !require_drm) {
        fprintf(stderr, "TH1520: dma-buf rendering failed, restarting OMX with NV12 output\n");
        force_nv12 = true;
        request_decoder_reset(false);
        result = reset_decoder();
      } else {
        fprintf(stderr, "TH1520: rendering failed in forced %s output mode\n", require_drm ? "DRM PRIME" : "NV12");
        request_decoder_reset(true);
      }
    }
    return result < 0 ? LOOP_RETURN : LOOP_OK;
  }
#endif
  if (ffmpeg_decoder == SOFTWARE) egl_draw(frame);
#ifdef HAVE_VAAPI
  else if (ffmpeg_decoder == VAAPI) vaapi_queue(frame, window, display_width, display_height);
#endif
  av_frame_free(&frame);
  return LOOP_OK;
}

int x11_init(bool vdpau, bool vaapi) {
  XInitThreads();
  if (!display) display = XOpenDisplay(NULL);
  if (!display) return 0;
#ifdef HAVE_VAAPI
  if (vaapi && vaapi_init_lib(display) == 0) return INIT_VAAPI;
#endif
  return INIT_EGL;
}

void x11_cleanup(void);

static int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  if (!display) {
    fprintf(stderr, "X11: cannot open display; set DISPLAY and XAUTHORITY for the desktop session\n");
    return -1;
  }
  stopping = false;
  decoded_frames = presented_frames = replaced_frames = 0;
  submit_calls = submit_total_us = submit_max_us = queue_max_us = 0;
  render_calls = render_total_us = render_max_us = 0;
  int avc_flags = SLICE_THREADING;
#ifdef HAVE_TH1520
  th1520_active = (drFlags & X11_TH1520_ACCELERATION) != 0;
  force_nv12 = reset_requested = await_idr = fatal_error = false;
  last_frame_valid = false;
  input_gaps = input_idrs = 0;
  trace_input = getenv("MOONLIGHT_TH1520_TRACE") != NULL;
  video_format = videoFormat; video_width = width; video_height = height;
  if (th1520_active) {
    const char* mode = getenv("MOONLIGHT_TH1520_OUTPUT");
    if (mode && strcmp(mode, "auto") && strcmp(mode, "drm") && strcmp(mode, "nv12")) {
      fprintf(stderr, "MOONLIGHT_TH1520_OUTPUT must be auto, drm or nv12\n");
      goto fail;
    }
    require_drm = mode && !strcmp(mode, "drm");
    force_nv12 = mode && !strcmp(mode, "nv12");
    avc_flags = TH1520_ACCELERATION | (force_nv12 ? TH1520_NV12 : 0);
    printf("TH1520 pipeline: render_control=independent h264_tail=%s h264_filler=%s packet_metadata=key+pts\n",
           videoFormat == VIDEO_FORMAT_H264 ? "max-two-zero" : "unchanged",
           videoFormat == VIDEO_FORMAT_H264 ? "skip-valid" : "unchanged");
  } else
#endif
  if (drFlags & X11_VDPAU_ACCELERATION) avc_flags = VDPAU_ACCELERATION;
  else if (drFlags & X11_VAAPI_ACCELERATION) avc_flags = VAAPI_ACCELERATION;

  display_width = (drFlags & DISPLAY_FULLSCREEN) ? WidthOfScreen(DefaultScreenOfDisplay(display)) : width;
  display_height = (drFlags & DISPLAY_FULLSCREEN) ? HeightOfScreen(DefaultScreenOfDisplay(display)) : height;
  XSetWindowAttributes attributes = {
    .event_mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask
  };
  window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, display_width, display_height,
                         0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &attributes);
  if (!window) goto fail;
  XStoreName(display, window, "Moonlight");
  XMapWindow(display, window);
  if (drFlags & DISPLAY_FULLSCREEN) {
    XEvent event = {0};
    event.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
  }
  XFlush(display);
  if (ffmpeg_init(videoFormat, width, height, avc_flags, 2, SLICES_PER_FRAME) < 0) goto fail;
  bool use_egl = ffmpeg_decoder == SOFTWARE;
#ifdef HAVE_TH1520
  use_egl |= th1520_active;
#endif
  if (use_egl) {
    if (egl_init(display, window, width, height) < 0) goto fail;
    egl_initialized = true;
    switch (drFlags & DISPLAY_ROTATE_MASK) {
    case DISPLAY_ROTATE_90: egl_set_rotation(90); break;
    case DISPLAY_ROTATE_180: egl_set_rotation(180); break;
    case DISPLAY_ROTATE_270: egl_set_rotation(270); break;
    }
  }
#ifdef HAVE_TH1520
  if (th1520_active && !force_nv12 && !egl_drm_prime_supported()) {
    const char* mode = getenv("MOONLIGHT_TH1520_OUTPUT");
    if (mode && !strcmp(mode, "drm")) {
      fprintf(stderr, "TH1520: forced DRM output requires EGL dma-buf import\n");
      goto fail;
    }
    fprintf(stderr, "TH1520: EGL dma-buf import unavailable; selecting NV12 output\n");
    force_nv12 = true;
    if (ffmpeg_init(videoFormat, width, height, TH1520_ACCELERATION | TH1520_NV12, 2, 1) < 0) goto fail;
  }
#endif
  if (pipe(pipefd) < 0) goto fail;
  if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(pipefd[1], F_SETFL, O_NONBLOCK) < 0) goto fail;
  fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  loop_add_fd(pipefd[0], frame_handle, POLLIN);
  x11_input_init(display, window, (drFlags & DISPLAY_VIEW_ONLY) != 0);
  input_initialized = true;
  return 0;
fail:
  x11_cleanup();
  return -1;
}

int x11_setup_vdpau(int format, int width, int height, int rate, void* context, int flags) {
  return x11_setup(format, width, height, rate, context, flags | X11_VDPAU_ACCELERATION);
}
int x11_setup_vaapi(int format, int width, int height, int rate, void* context, int flags) {
  return x11_setup(format, width, height, rate, context, flags | X11_VAAPI_ACCELERATION);
}
#ifdef HAVE_TH1520
static int x11_setup_th1520(int format, int width, int height, int rate, void* context, int flags) {
  return x11_setup(format, width, height, rate, context, flags | X11_TH1520_ACCELERATION);
}
#endif

static void x11_stop(void) {
  pthread_mutex_lock(&state_mutex);
  stopping = true;
  pthread_mutex_unlock(&state_mutex);
}

void x11_cleanup(void) {
  x11_stop();
  pthread_mutex_lock(&decoder_mutex);
  AVFrame* frame = take_frame();
  av_frame_free(&frame);
  // Release GPU references while the old decoder is still alive.
  if (egl_initialized) { egl_destroy(); egl_initialized = false; }
  ffmpeg_destroy();
  if (input_initialized) { x11_input_cleanup(); input_initialized = false; }
  if (pipefd[0] >= 0) { loop_remove_fd(pipefd[0]); close(pipefd[0]); }
  if (pipefd[1] >= 0) close(pipefd[1]);
  pipefd[0] = pipefd[1] = -1;
  if (display) {
    if (window) XDestroyWindow(display, window);
    XCloseDisplay(display);
    display = NULL; window = 0;
  }
  free(ffmpeg_buffer); ffmpeg_buffer = NULL; ffmpeg_buffer_size = 0;
#ifdef HAVE_TH1520
  if (th1520_active) {
    printf("TH1520 frames: decoded=%lu presented=%lu replaced=%lu\n", decoded_frames, presented_frames, replaced_frames);
    printf("TH1520 timing: submit_avg_ms=%.3f submit_max_ms=%.3f queue_max_ms=%.3f render_avg_ms=%.3f render_max_ms=%.3f input_gaps=%lu input_idrs=%lu\n",
           submit_calls ? submit_total_us / (1000.0 * submit_calls) : 0.0, submit_max_us / 1000.0, queue_max_us / 1000.0,
           render_calls ? render_total_us / (1000.0 * render_calls) : 0.0, render_max_us / 1000.0, input_gaps, input_idrs);
  }
  th1520_active = false;
#endif
  pthread_mutex_unlock(&decoder_mutex);
}

#ifdef HAVE_TH1520
static int th1520_submit(PDECODE_UNIT unit) {
  bool key = unit->frameType == FRAME_TYPE_IDR;
  pthread_mutex_lock(&state_mutex);
  bool waiting = await_idr;
  if (waiting && key) await_idr = false;
  pthread_mutex_unlock(&state_mutex);
  if (waiting && !key) return DR_NEED_IDR;

  uint32_t number = (uint32_t)unit->frameNumber;
  if (last_frame_valid && number != last_frame_number + 1) {
    input_gaps++;
    fprintf(stderr, "TH1520 input gap: previous=%" PRIu32 " current=%" PRIu32 " type=%s\n",
            last_frame_number, number, key ? "IDR" : "P");
    // Core queue overflow normally resumes at IDR. A P-frame gap is not safe
    // for this backend (no reference invalidation): don't feed missing refs.
    if (!key) { request_decoder_reset(false); return DR_NEED_IDR; }
  }
  unsigned char* data = ffmpeg_buffer;
  int length = unit->fullLength;
  if (trace_input && video_format == VIDEO_FORMAT_H264 && submit_calls < 5)
    trace_h264_input(data, length, unit->frameNumber);
  int64_t pts = unit->presentationTimeUs <= INT64_MAX ? (int64_t)unit->presentationTimeUs : AV_NOPTS_VALUE;
  for (int step = 0; step < MAX_DECODE_STEPS; step++) {
    int control = submission_state();
    if (control != 0) return control > 0 ? DR_OK : DR_NEED_IDR;
    AVFrame* frame = NULL;
    int status = ffmpeg_decode_frame_ex(data, length, key, pts, &frame);
    if (frame) {
      queue_frame(frame, unit->colorspace);
      av_frame_unref(frame); // Return the borrowed ring reference immediately.
    }
    if (status == AVERROR_INPUT_CHANGED) {
      fprintf(stderr, "TH1520: host changed video dimensions; restart streaming at the new resolution\n");
      request_decoder_reset(true);
      return DR_NEED_IDR;
    }
    if (status < 0) break;
    data = NULL; length = 0; key = false; pts = AV_NOPTS_VALUE;
    if (!(status & (FFMPEG_DECODE_PACKET_PENDING | FFMPEG_DECODE_FRAME_AVAILABLE))) {
      last_frame_valid = true;
      last_frame_number = number;
      if (unit->frameType == FRAME_TYPE_IDR) input_idrs++;
      return DR_OK;
    }
  }
  // A bounded pump must never return DR_OK while silently skipping the next AU.
  fprintf(stderr, "TH1520: decoder error/backpressure at frame %" PRIu32 ", requesting restart and IDR\n", number);
  request_decoder_reset(false);
  return DR_NEED_IDR;
}
#endif

int x11_submit_decode_unit(PDECODE_UNIT unit) {
  if (!unit || unit->fullLength <= 0) return DR_NEED_IDR;
  int control = submission_state();
  if (control != 0) return control > 0 ? DR_OK : DR_NEED_IDR;
  uint64_t begin = monotonic_us();
  // DU enqueue time uses moonlight-common's process-relative clock, not the
  // absolute CLOCK_MONOTONIC value used for local duration measurements.
  uint64_t common_now = unit->enqueueTimeUs ? LiGetMicroseconds() : 0;
  uint64_t queue_age = unit->enqueueTimeUs && common_now >= unit->enqueueTimeUs ? common_now - unit->enqueueTimeUs : 0;
  pthread_mutex_lock(&decoder_mutex);
  control = submission_state();
  if (control != 0) { pthread_mutex_unlock(&decoder_mutex); return control > 0 ? DR_OK : DR_NEED_IDR; }
  int result = DR_NEED_IDR;
  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, (size_t)unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
  int length = 0;
  for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
    if (entry->length < 0 || entry->length > unit->fullLength - length || !entry->data) goto finished;
    memcpy((unsigned char*)ffmpeg_buffer + length, entry->data, entry->length);
    length += entry->length;
  }
  if (length != unit->fullLength) goto finished;
  memset((unsigned char*)ffmpeg_buffer + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  result = DR_OK;
#ifdef HAVE_TH1520
  if (th1520_active) result = th1520_submit(unit);
  else
#endif
  {
    if (ffmpeg_decode(ffmpeg_buffer, length) < 0) result = DR_NEED_IDR;
    AVFrame* frame = ffmpeg_get_frame(true);
    if (frame) queue_frame(frame, unit->colorspace);
  }
finished:
  {
    uint64_t end = monotonic_us();
    uint64_t elapsed = end >= begin ? end - begin : 0;
    submit_calls++; submit_total_us += elapsed;
    if (elapsed > submit_max_us) submit_max_us = elapsed;
    if (queue_age > queue_max_us) queue_max_us = queue_age;
#ifdef HAVE_TH1520
    if (th1520_active && (elapsed >= 50000 || queue_age >= 100000))
      fprintf(stderr, "TH1520 slow submit: frame=%d type=%s bytes=%d elapsed_ms=%.3f queue_ms=%.3f\n",
              unit->frameNumber, unit->frameType == FRAME_TYPE_IDR ? "IDR" : "P", length, elapsed / 1000.0, queue_age / 1000.0);
#endif
  }
  pthread_mutex_unlock(&decoder_mutex);
  return result;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup, .stop = x11_stop, .cleanup = x11_cleanup, .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
};
DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vdpau = {
  .setup = x11_setup_vdpau, .stop = x11_stop, .cleanup = x11_cleanup, .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
  .setup = x11_setup_vaapi, .stop = x11_stop, .cleanup = x11_cleanup, .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
#ifdef HAVE_TH1520
DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_th1520 = {
  .setup = x11_setup_th1520, .stop = x11_stop, .cleanup = x11_cleanup, .submitDecodeUnit = x11_submit_decode_unit,
  // Vendor OMX calls can wait: let moonlight-common's decoder thread, rather
  // than its network receive thread, perform the submission.
  .capabilities = 0,
};
#endif
