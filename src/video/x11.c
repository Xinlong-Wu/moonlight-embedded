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
static pthread_mutex_t decoder_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_TH1520
static bool th1520_active, force_nv12, require_drm, reset_requested, await_idr, fatal_error;
static int video_format, video_width, video_height;
#endif

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
// Called on the rendering thread with decoder_mutex held. All external OMX
// references must be returned before creating another OMX component.
static int reset_decoder(void) {
  AVFrame* queued = take_frame();
  av_frame_free(&queued);
  if (egl_release_frame() < 0) {
    fprintf(stderr, "TH1520: cannot retire GPU frame; refusing decoder restart\n");
    fatal_error = true;
    return -1;
  }
  reset_requested = false;
  await_idr = true;
  if (ffmpeg_init(video_format, video_width, video_height,
                  TH1520_ACCELERATION | (force_nv12 ? TH1520_NV12 : 0), 2, 1) < 0) {
    fprintf(stderr, "TH1520: decoder restart failed (no software fallback)\n");
    fatal_error = true;
    return -1;
  }
  return 0;
}
#endif

static int frame_handle(int fd) {
  char tokens[128];
  while (read(fd, tokens, sizeof(tokens)) > 0) {}
#ifdef HAVE_TH1520
  pthread_mutex_lock(&decoder_mutex);
  if (th1520_active && fatal_error) {
    pthread_mutex_unlock(&decoder_mutex);
    return LOOP_RETURN;
  }
  if (th1520_active && reset_requested) {
    int result = reset_decoder();
    pthread_mutex_unlock(&decoder_mutex);
    return result < 0 ? LOOP_RETURN : LOOP_OK;
  }
  pthread_mutex_unlock(&decoder_mutex);
#endif
  AVFrame* frame = take_frame();
  if (!frame) return LOOP_OK;
#ifdef HAVE_TH1520
  int result = 0;
  if (th1520_active) {
    if (frame->format == AV_PIX_FMT_DRM_PRIME) result = egl_draw_drm_prime(frame);
    else if (frame->format == AV_PIX_FMT_NV12) result = egl_draw_nv12(frame);
    else result = -1;
    if (result == 0) presented_frames++;
    av_frame_free(&frame);
    if (result < 0) {
      pthread_mutex_lock(&decoder_mutex);
      if (!force_nv12 && !require_drm) {
        fprintf(stderr, "TH1520: dma-buf rendering failed, restarting OMX with NV12 output\n");
        force_nv12 = true;
        reset_requested = true;
        result = reset_decoder();
      } else {
        fprintf(stderr, "TH1520: rendering failed in forced %s output mode\n", require_drm ? "DRM PRIME" : "NV12");
        fatal_error = true;
      }
      pthread_mutex_unlock(&decoder_mutex);
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
  int avc_flags = SLICE_THREADING;
#ifdef HAVE_TH1520
  th1520_active = (drFlags & X11_TH1520_ACCELERATION) != 0;
  force_nv12 = reset_requested = await_idr = fatal_error = false;
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
  pthread_mutex_lock(&decoder_mutex);
  stopping = true;
  pthread_mutex_unlock(&decoder_mutex);
}

void x11_cleanup(void) {
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
  if (th1520_active)
    printf("TH1520 frames: decoded=%lu presented=%lu replaced=%lu\n", decoded_frames, presented_frames, replaced_frames);
  th1520_active = false;
#endif
}

#ifdef HAVE_TH1520
static int th1520_submit(PDECODE_UNIT unit) {
  if (fatal_error || reset_requested) return DR_NEED_IDR;
  if (await_idr) {
    if (unit->frameType != FRAME_TYPE_IDR) return DR_NEED_IDR;
    await_idr = false;
  }
  unsigned char* data = ffmpeg_buffer;
  int length = unit->fullLength;
  for (int step = 0; step < MAX_DECODE_STEPS; step++) {
    AVFrame* frame = NULL;
    int status = ffmpeg_decode_frame(data, length, &frame);
    if (frame) {
      queue_frame(frame, unit->colorspace);
      av_frame_unref(frame); // Return the borrowed ring reference immediately.
    }
    if (status == AVERROR_INPUT_CHANGED) {
      fprintf(stderr, "TH1520: host changed video dimensions; restart streaming at the new resolution\n");
      fatal_error = true;
      notify_renderer();
      return DR_NEED_IDR;
    }
    if (status < 0) break;
    data = NULL; length = 0;
    if (!(status & (FFMPEG_DECODE_PACKET_PENDING | FFMPEG_DECODE_FRAME_AVAILABLE))) return DR_OK;
  }
  // A bounded pump must never return DR_OK while silently skipping the next AU.
  // Restart on the UI thread after returning all mailbox/GPU references.
  fprintf(stderr, "TH1520: decoder error/backpressure, restarting and requesting IDR\n");
  reset_requested = true;
  notify_renderer();
  return DR_NEED_IDR;
}
#endif

int x11_submit_decode_unit(PDECODE_UNIT unit) {
  if (!unit || unit->fullLength <= 0) return DR_NEED_IDR;
  pthread_mutex_lock(&decoder_mutex);
  if (stopping) { pthread_mutex_unlock(&decoder_mutex); return DR_OK; }
  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, (size_t)unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
  int length = 0;
  for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
    if (entry->length < 0 || entry->length > unit->fullLength - length || !entry->data) {
      pthread_mutex_unlock(&decoder_mutex); return DR_NEED_IDR;
    }
    memcpy((unsigned char*)ffmpeg_buffer + length, entry->data, entry->length);
    length += entry->length;
  }
  if (length != unit->fullLength) { pthread_mutex_unlock(&decoder_mutex); return DR_NEED_IDR; }
  memset((unsigned char*)ffmpeg_buffer + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
  int result = DR_OK;
#ifdef HAVE_TH1520
  if (th1520_active) result = th1520_submit(unit);
  else
#endif
  {
    if (ffmpeg_decode(ffmpeg_buffer, length) < 0) result = DR_NEED_IDR;
    AVFrame* frame = ffmpeg_get_frame(true);
    if (frame) queue_frame(frame, unit->colorspace);
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
