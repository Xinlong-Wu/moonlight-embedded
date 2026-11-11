/* CPU-only lock regression: presentation/stop must not wait for vendor decode.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Production x11.c is included to exercise its real mailbox/control path;
 * FFmpeg submissions, EGL and input are stubbed, so no device/display is opened.
 */
#include "../src/video/x11.c"
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); exit(1); \
} } while (0)

// Link-time dependencies of setup/restart paths, which this test does not enter.
enum decoders ffmpeg_decoder = TH1520_OMX;
int ffmpeg_init(int f, int w, int h, int p, int b, int t) { return 0; }
void ffmpeg_destroy(void) {}
int ffmpeg_decode(unsigned char* data, int size) { return 0; }
AVFrame* ffmpeg_get_frame(bool native) { return NULL; }
int ffmpeg_decode_frame_ex(unsigned char* data, int size, bool key, int64_t pts, AVFrame** frame) {
  *frame = NULL;
  return 0;
}
int egl_init(EGLNativeDisplayType d, EGLNativeWindowType w, int x, int y) { return 0; }
void egl_draw(const AVFrame* f) {}
void egl_set_rotation(int r) {}
void egl_destroy(void) {}
int egl_draw_nv12(const AVFrame* f) { return 0; }
int egl_draw_drm_prime(const AVFrame* f) { return 0; }
bool egl_drm_prime_supported(void) { return true; }
int egl_release_frame(void) { return 0; }
void x11_input_init(Display* d, Window w, bool viewonly) {}
void x11_input_cleanup(void) {}
void loop_add_fd(int fd, FdHandler callback, int events) {}
void loop_remove_fd(int fd) {}
uint64_t LiGetMicroseconds(void) { return 0; }
bool ensure_buf_size(void** buffer, size_t* capacity, size_t required) {
  if (*capacity >= required) return false;
  *buffer = realloc(*buffer, required);
  if (!*buffer) abort();
  *capacity = required;
  return true;
}

struct coordination {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool vendor_entered, release_vendor, renderer_done;
  int render_result;
};

static void* blocked_vendor(void* opaque) {
  struct coordination* c = opaque;
  pthread_mutex_lock(&decoder_mutex);
  pthread_mutex_lock(&c->mutex);
  c->vendor_entered = true;
  pthread_cond_broadcast(&c->cond);
  while (!c->release_vendor) pthread_cond_wait(&c->cond, &c->mutex);
  pthread_mutex_unlock(&c->mutex);
  pthread_mutex_unlock(&decoder_mutex);
  return NULL;
}

static void* render_and_stop(void* opaque) {
  struct coordination* c = opaque;
  int result = frame_handle(pipefd[0]);
  // stop() must also be a short state transition, not a wait for vendor OMX.
  x11_stop();
  pthread_mutex_lock(&c->mutex);
  c->render_result = result;
  c->renderer_done = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mutex);
  return NULL;
}

int main(void) {
  for (int run = 0; run < 20; run++) {
    struct coordination c = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
    pthread_t vendor, renderer;
    stopping = false;
    th1520_active = true;
    reset_requested = fatal_error = false;
    force_nv12 = true;
    decoded_frames = presented_frames = replaced_frames = 0;
    CHECK(pipe(pipefd) == 0);
    CHECK(fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == 0);
    AVFrame* frame = av_frame_alloc();
    CHECK(frame != NULL);
    frame->format = AV_PIX_FMT_NV12;
    frame->width = frame->height = 4;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    queue_frame(frame, COLORSPACE_REC_709);

    CHECK(pthread_create(&vendor, NULL, blocked_vendor, &c) == 0);
    pthread_mutex_lock(&c.mutex);
    while (!c.vendor_entered) pthread_cond_wait(&c.cond, &c.mutex);
    pthread_mutex_unlock(&c.mutex);
    CHECK(pthread_create(&renderer, NULL, render_and_stop, &c) == 0);

    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 250000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&c.mutex);
    int wait_result = 0;
    while (!c.renderer_done && wait_result == 0)
      wait_result = pthread_cond_timedwait(&c.cond, &c.mutex, &deadline);
    bool independent = c.renderer_done;
    c.release_vendor = true;
    pthread_cond_broadcast(&c.cond);
    pthread_mutex_unlock(&c.mutex);
    CHECK(pthread_join(vendor, NULL) == 0);
    CHECK(pthread_join(renderer, NULL) == 0);
    if (!independent || c.render_result != LOOP_OK || presented_frames != 1 || queued_frame != NULL) {
      fprintf(stderr, "FAIL: renderer/stop waited for decoder_mutex (run %d)\n", run + 1);
      return 1;
    }
    av_frame_free(&frame);
    close(pipefd[0]); close(pipefd[1]);
    pipefd[0] = pipefd[1] = -1;
    pthread_mutex_destroy(&c.mutex);
    pthread_cond_destroy(&c.cond);
  }
  puts("PASS: presentation and stop stay responsive while decoder_mutex is held (20 runs)");
  return 0;
}
