/*
 * TH1520 production decoder/renderer integration test.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This intentionally replaces loop.c, not the production video callbacks.
 * Build with HAVE_X11 and HAVE_TH1520, the production x11/egl/ffmpeg video
 * sources, input/x11.c, util.c, and moonlight-common. No host is contacted.
 *
 * Usage: th1520-test input.h264 h264|h265 width height fps [repetitions]
 * MOONLIGHT_TH1520_OUTPUT=drm|nv12 selects the production output mode.
 * Each repetition submits at most three seconds of the input, then services
 * pending UI messages for another 250 ms. Supply a colorful Annex-B sample
 * beginning with an IDR and its SPS/PPS (and VPS for HEVC).
 * Optional: MOONLIGHT_TH1520_SCREENSHOT=output.ppm saves the final X11 image.
 * TEST_TAIL_PADDING=0..1024, TEST_BURST=1..32 and TEST_FULLSCREEN=1 (all
 * prefixed MOONLIGHT_TH1520_) cover real-transport framing/presentation cases.
 * Run under an external timeout: vendor OMX calls may themselves block.
 */
#define _POSIX_C_SOURCE 200809L

#include "../src/video/video.h"
#include "../src/loop.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_HANDLERS 32
#define MAX_INPUT_BYTES (256U * 1024U * 1024U)
#define MAX_NALS 65536
#define TEST_SECONDS 3

struct handler {
  int fd;
  int events;
  FdHandler callback;
};
static struct handler handlers[MAX_HANDLERS];
static size_t handler_count;
static bool loop_failed;
static unsigned x_errors;
static bool threaded_mode;
static int render_delay_ms;
static int tail_padding_bytes;
static int burst_size = 1;
static bool test_fullscreen;
static const uint8_t zero_tail[1024];
static pthread_mutex_t producer_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool producer_done, producer_cancelled;
static int producer_result;

static bool cancelled(void) {
  pthread_mutex_lock(&producer_mutex);
  bool result = producer_cancelled;
  pthread_mutex_unlock(&producer_mutex);
  return result;
}

void loop_add_fd(int fd, FdHandler callback, int events) {
  if (fd < 0 || !callback || handler_count == MAX_HANDLERS) {
    fprintf(stderr, "FAIL: invalid or overflowing event registration\n");
    loop_failed = true;
    return;
  }
  for (size_t i = 0; i < handler_count; i++) {
    if (handlers[i].fd == fd) {
      fprintf(stderr, "FAIL: duplicate event fd %d\n", fd);
      loop_failed = true;
      return;
    }
  }
  handlers[handler_count++] = (struct handler) { fd, events, callback };
}

void loop_remove_fd(int fd) {
  for (size_t i = 0; i < handler_count; i++) {
    if (handlers[i].fd == fd) {
      handlers[i] = handlers[--handler_count];
      return;
    }
  }
  // Setup failure cleanup may close an fd before it was registered.
}

static uint64_t monotonic_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
    perror("clock_gettime");
    exit(2);
  }
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int dispatch_until(uint64_t deadline) {
  bool first = true;
  while (first || monotonic_ns() < deadline) {
    first = false;
    if (loop_failed || x_errors) return -1;
    struct pollfd fds[MAX_HANDLERS];
    size_t count = handler_count;
    for (size_t i = 0; i < count; i++)
      fds[i] = (struct pollfd) { .fd = handlers[i].fd, .events = (short)handlers[i].events };
    uint64_t now = monotonic_ns();
    int timeout = now >= deadline ? 0 : (int)((deadline - now + 999999) / 1000000);
    int result = poll(fds, count, timeout);
    if (result < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      return -1;
    }
    for (size_t i = 0; i < count; i++) {
      if (!fds[i].revents) continue;
      if (fds[i].revents & POLLNVAL) {
        fprintf(stderr, "FAIL: event loop contains closed fd %d\n", fds[i].fd);
        return -1;
      }
      // Dispatch a snapshot safely even if a callback unregisters another fd.
      FdHandler callback = NULL;
      for (size_t j = 0; j < handler_count; j++)
        if (handlers[j].fd == fds[i].fd) callback = handlers[j].callback;
      if (callback && callback(fds[i].fd) != LOOP_OK) {
        fprintf(stderr, "FAIL: renderer/input requested event-loop termination\n");
        return -1;
      }
      if (fds[i].revents & (POLLERR | POLLHUP)) {
        fprintf(stderr, "FAIL: event fd %d reported error/hangup\n", fds[i].fd);
        return -1;
      }
    }
  }
  return loop_failed || x_errors ? -1 : 0;
}

static int x_error_handler(Display* display, XErrorEvent* event) {
  char message[128];
  XGetErrorText(display, event->error_code, message, sizeof(message));
  fprintf(stderr, "FAIL: X11 error %s (request %u, resource 0x%lx)\n",
          message, event->request_code, event->resourceid);
  x_errors++;
  return 0;
}

static long count_fds(void) {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) { perror("opendir /proc/self/fd"); return -1; }
  int own_fd = dirfd(directory);
  long count = 0;
  struct dirent* entry;
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    char* end;
    long fd = strtol(entry->d_name, &end, 10);
    if (*entry->d_name && !*end && fd != own_fd) count++;
  }
  int saved_errno = errno;
  if (closedir(directory) < 0 || saved_errno) {
    if (saved_errno) errno = saved_errno;
    perror("read /proc/self/fd");
    return -1;
  }
  return count;
}

static long rss_kib(void) {
  FILE* file = fopen("/proc/self/statm", "r");
  if (!file) { perror("fopen /proc/self/statm"); return -1; }
  unsigned long size, resident;
  int matched = fscanf(file, "%lu %lu", &size, &resident);
  int close_result = fclose(file);
  long page_size = sysconf(_SC_PAGESIZE);
  if (matched != 2 || close_result || page_size <= 0 || resident > (unsigned long)LONG_MAX / (unsigned long)page_size) {
    fprintf(stderr, "FAIL: cannot measure process RSS\n");
    return -1;
  }
  return (long)(resident * (unsigned long)page_size / 1024);
}

static Window find_moonlight(Display* display, Window parent, unsigned depth) {
  if (depth > 10) return None;
  Window root, ancestor, *children = NULL;
  unsigned count = 0;
  if (!XQueryTree(display, parent, &root, &ancestor, &children, &count)) return None;
  Window found = None;
  // XQueryTree lists bottom-to-top: prefer the newly created topmost window.
  for (unsigned i = count; i > 0 && found == None; i--) {
    Window child = children[i - 1];
    char* name = NULL;
    if (XFetchName(display, child, &name) && name) {
      XWindowAttributes attributes;
      if (!strcmp(name, "Moonlight") && XGetWindowAttributes(display, child, &attributes) &&
          attributes.map_state == IsViewable && attributes.width > 1 && attributes.height > 1)
        found = child;
      XFree(name);
    }
    if (found == None) found = find_moonlight(display, child, depth + 1);
  }
  if (children) XFree(children);
  return found;
}

static unsigned component(unsigned long pixel, unsigned long mask) {
  if (!mask) return 0;
  while (!(mask & 1)) { pixel >>= 1; mask >>= 1; }
  return (unsigned)(((pixel & mask) * 255UL + mask / 2) / mask);
}

static int write_ppm(const char* path, XImage* image) {
  FILE* file = fopen(path, "wb");
  if (!file) { perror("fopen screenshot"); return -1; }
  int result = fprintf(file, "P6\n%d %d\n255\n", image->width, image->height) < 0 ? -1 : 0;
  uint8_t* row = malloc((size_t)image->width * 3);
  if (!row) result = -1;
  for (int y = 0; y < image->height && result == 0; y++) {
    for (int x = 0; x < image->width; x++) {
      unsigned long pixel = XGetPixel(image, x, y);
      row[x * 3] = (uint8_t)component(pixel, image->red_mask);
      row[x * 3 + 1] = (uint8_t)component(pixel, image->green_mask);
      row[x * 3 + 2] = (uint8_t)component(pixel, image->blue_mask);
    }
    if (fwrite(row, 3, (size_t)image->width, file) != (size_t)image->width) result = -1;
  }
  free(row);
  if (fclose(file)) result = -1;
  if (result < 0) fprintf(stderr, "FAIL: writing PPM screenshot %s\n", path);
  else printf("SCREENSHOT %s\n", path);
  return result;
}

static int check_picture(Display* display, const char* screenshot) {
  XSync(display, False);
  Window window = find_moonlight(display, DefaultRootWindow(display), 0);
  if (window == None) {
    fprintf(stderr, "FAIL: no mapped Moonlight window found\n");
    return -1;
  }
  XWindowAttributes attributes;
  if (!XGetWindowAttributes(display, window, &attributes)) return -1;
  XImage* image = XGetImage(display, window, 0, 0, (unsigned)attributes.width,
                            (unsigned)attributes.height, AllPlanes, ZPixmap);
  if (!image || x_errors) {
    fprintf(stderr, "FAIL: cannot capture Moonlight X11 image\n");
    if (image) XDestroyImage(image);
    return -1;
  }
  unsigned sampled = 0, nonblack = 0, colorful = 0;
  unsigned min_luma = UINT_MAX, max_luma = 0;
  int x_step = image->width > 128 ? image->width / 128 : 1;
  int y_step = image->height > 128 ? image->height / 128 : 1;
  for (int y = y_step / 2; y < image->height; y += y_step) {
    for (int x = x_step / 2; x < image->width; x += x_step) {
      unsigned long pixel = XGetPixel(image, x, y);
      unsigned r = component(pixel, image->red_mask);
      unsigned g = component(pixel, image->green_mask);
      unsigned b = component(pixel, image->blue_mask);
      unsigned high = r > g ? r : g;
      unsigned low = r < g ? r : g;
      if (b > high) high = b;
      if (b < low) low = b;
      unsigned luma = r + g + b;
      if (luma < min_luma) min_luma = luma;
      if (luma > max_luma) max_luma = luma;
      sampled++;
      if (high > 16) nonblack++;
      if (high > 32 && high - low > 24) colorful++;
    }
  }
  printf("PICTURE window=0x%lx size=%dx%d samples=%u nonblack=%u colorful=%u luma_sum_range=%u..%u\n",
         window, image->width, image->height, sampled, nonblack, colorful, min_luma, max_luma);
  int result = 0;
  if (nonblack == 0 || max_luma - min_luma < 48 || colorful < sampled / 100) {
    fprintf(stderr, "FAIL: rendered test pattern is black, flat or lacks color variation\n");
    result = -1;
  }
  if (screenshot && *screenshot && write_ppm(screenshot, image) < 0) result = -1;
  XDestroyImage(image);
  return result;
}

struct parameter_set {
  uint8_t* data;
  int length;
};
struct session {
  DECODER_RENDERER_CALLBACKS* callbacks;
  struct parameter_set parameters[3]; // SPS, PPS, VPS
  bool hevc;
  bool started;
  int fps;
  unsigned submitted;
  unsigned limit;
  uint64_t start_ns;
};

static int parameter_index(bool hevc, unsigned type) {
  if (hevc) {
    if (type == 33) return 0;
    if (type == 34) return 1;
    if (type == 32) return 2;
  } else {
    if (type == 7) return 0;
    if (type == 8) return 1;
  }
  return -1;
}

static size_t next_start_code(const uint8_t* data, size_t size, size_t offset, size_t* prefix) {
  for (size_t i = offset; i + 3 < size; i++) {
    if (data[i] || data[i + 1]) continue;
    if (data[i + 2] == 1) { *prefix = 3; return i; }
    if (!data[i + 2] && data[i + 3] == 1) { *prefix = 4; return i; }
  }
  return size;
}

static int save_parameter(struct parameter_set* parameter, const uint8_t* data, size_t length) {
  if (!length || length > INT_MAX) return -1;
  uint8_t* copy = av_memdup(data, length);
  if (!copy) return -1;
  av_free(parameter->data);
  parameter->data = copy;
  parameter->length = (int)length;
  return 0;
}

// av_parser_parse2() supplies a complete Annex-B AU. Split it into the same
// configuration/PICDATA linked-list shape moonlight-common supplies for IDRs.
static int submit_au(struct session* session, const uint8_t* data, int size) {
  if (cancelled()) return -1;
  if (size <= 0) return 0;
  size_t prefix = 0;
  size_t position = next_start_code(data, (size_t)size, 0, &prefix);
  if (position == (size_t)size) {
    fprintf(stderr, "FAIL: parser produced a non-Annex-B access unit\n");
    return -1;
  }
  // A NAL needs at least a start code plus one header byte; this bounds count.
  size_t capacity = (size_t)size / 4 + 4;
  if (capacity > MAX_NALS + 3) capacity = MAX_NALS + 3;
  // Reserve one extra PICDATA entry for transport-style tail padding.
  LENTRY* entries = calloc(capacity + 1, sizeof(*entries));
  if (!entries) return -1;
  size_t body_count = 0;
  bool key = false, vcl = false, has_configuration = false;
  while (position < (size_t)size) {
    size_t header = position + prefix;
    if (header >= (size_t)size || (session->hevc && header + 1 >= (size_t)size)) goto malformed;
    unsigned type = session->hevc ? (data[header] >> 1) & 0x3f : data[header] & 0x1f;
    size_t next_prefix = 0;
    size_t next = next_start_code(data, (size_t)size, header + 1, &next_prefix);
    size_t length = next - position;
    int parameter = parameter_index(session->hevc, type);
    if (parameter >= 0) {
      if (save_parameter(&session->parameters[parameter], data + position, length) < 0) goto malformed;
      has_configuration = true;
    } else if ((!session->hevc && type != 9) || (session->hevc && type != 35)) {
      if (body_count + 3 >= capacity || body_count >= MAX_NALS || length > INT_MAX) goto malformed;
      entries[3 + body_count++] = (LENTRY) {
        .data = (char*)data + position, .length = (int)length, .bufferType = BUFFER_TYPE_PICDATA
      };
    }
    if (session->hevc) {
      vcl |= type <= 31;
      key |= type >= 16 && type <= 21;
    } else {
      vcl |= type >= 1 && type <= 5;
      key |= type == 5;
    }
    position = next;
    prefix = next_prefix;
  }
  if (!vcl) { free(entries); return 0; }
  if (!session->started && !key) { free(entries); return 0; }
  if (key && (!session->parameters[0].data || !session->parameters[1].data ||
              (session->hevc && !session->parameters[2].data))) {
    fprintf(stderr, "FAIL: IDR missing SPS/PPS%s\n", session->hevc ? "/VPS" : "");
    free(entries);
    return -1;
  }
  size_t first = 3;
  if (key || has_configuration) {
    if (session->parameters[1].data)
      entries[--first] = (LENTRY) { .data = (char*)session->parameters[1].data, .length = session->parameters[1].length, .bufferType = BUFFER_TYPE_PPS };
    if (session->parameters[0].data)
      entries[--first] = (LENTRY) { .data = (char*)session->parameters[0].data, .length = session->parameters[0].length, .bufferType = BUFFER_TYPE_SPS };
    if (session->hevc && session->parameters[2].data)
      entries[--first] = (LENTRY) { .data = (char*)session->parameters[2].data, .length = session->parameters[2].length, .bufferType = BUFFER_TYPE_VPS };
  }
  size_t entry_end = 3 + body_count;
  if (tail_padding_bytes) {
    entries[entry_end++] = (LENTRY) { .data = (char*)zero_tail, .length = tail_padding_bytes, .bufferType = BUFFER_TYPE_PICDATA };
  }
  int full_length = 0;
  for (size_t i = first; i < entry_end; i++) {
    if (entries[i].length <= 0 || entries[i].length > INT_MAX - full_length) goto malformed;
    full_length += entries[i].length;
    entries[i].next = i + 1 < entry_end ? &entries[i + 1] : NULL;
  }
  if (!session->started) {
    session->start_ns = monotonic_ns();
    session->started = true;
  }
  DECODE_UNIT unit = {0};
  unit.frameNumber = (int)session->submitted + 1;
  unit.frameType = key ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME;
  unit.fullLength = full_length;
  unit.bufferList = &entries[first];
  unit.presentationTimeUs = (uint64_t)session->submitted * 1000000 / (unsigned)session->fps;
  unit.rtpTimestamp = (uint32_t)((uint64_t)session->submitted * 90000 / (unsigned)session->fps);
  unit.receiveTimeUs = unit.enqueueTimeUs = LiGetMicroseconds();
  unit.colorspace = COLORSPACE_REC_709;
  int result = session->callbacks->submitDecodeUnit(&unit);
  free(entries);
  if (result != DR_OK) {
    fprintf(stderr, "FAIL: submitDecodeUnit returned %d at AU %u%s\n", result,
            session->submitted + 1, result == DR_NEED_IDR ? " (requested IDR/recovery)" : "");
    return -1;
  }
  session->submitted++;
  uint64_t deadline = session->start_ns + (uint64_t)session->submitted * 1000000000 / (unsigned)session->fps;
  bool pace = session->submitted % (unsigned)burst_size == 0 || session->submitted >= session->limit;
  if (threaded_mode && pace) {
    uint64_t now;
    while ((now = monotonic_ns()) < deadline && !cancelled()) {
      uint64_t ns = deadline - now;
      struct timespec delay = { .tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000 };
      if (nanosleep(&delay, NULL) < 0 && errno != EINTR) return -1;
    }
  } else if (!threaded_mode && dispatch_until(pace ? deadline : monotonic_ns()) < 0) return -1;
  if (session->submitted % ((unsigned)session->fps * 10) == 0)
    printf("STREAM submitted=%u elapsed_ms=%" PRIu64 " rss_kib=%ld fd_count=%ld\n",
           session->submitted, (monotonic_ns() - session->start_ns) / 1000000, rss_kib(), count_fds());
  return session->submitted >= session->limit ? 1 : 0;
malformed:
  fprintf(stderr, "FAIL: malformed/oversized AU or parameter allocation failure\n");
  free(entries);
  return -1;
}

static int play_input(struct session* session, const uint8_t* data, size_t length) {
  enum AVCodecID codec = session->hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
  AVCodecParserContext* parser = av_parser_init(codec);
  AVCodecContext* context = avcodec_alloc_context3(NULL);
  if (!parser || !context) {
    fprintf(stderr, "FAIL: cannot allocate Annex-B parser\n");
    if (parser) av_parser_close(parser);
    avcodec_free_context(&context);
    return -1;
  }
  context->codec_type = AVMEDIA_TYPE_VIDEO;
  context->codec_id = codec;
  size_t offset = 0;
  int result = 0;
  while (offset < length && session->submitted < session->limit) {
    uint8_t* output = NULL;
    int output_size = 0;
    int input_size = (int)((length - offset) > 65536 ? 65536 : length - offset);
    int consumed = av_parser_parse2(parser, context, &output, &output_size,
                                    data + offset, input_size, AV_NOPTS_VALUE,
                                    AV_NOPTS_VALUE, (int64_t)offset);
    if (consumed < 0 || consumed > input_size || (!consumed && !output_size)) {
      fprintf(stderr, "FAIL: Annex-B parser made no progress or returned error %d\n", consumed);
      result = -1;
      break;
    }
    offset += (size_t)consumed;
    if (output_size > 0) {
      result = submit_au(session, output, output_size);
      if (result != 0) break;
    }
  }
  if (result == 0 && session->submitted < session->limit) {
    uint8_t* output = NULL;
    int output_size = 0;
    if (av_parser_parse2(parser, context, &output, &output_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, (int64_t)offset) < 0)
      result = -1;
    else if (output_size > 0)
      result = submit_au(session, output, output_size);
  }
  av_parser_close(parser);
  avcodec_free_context(&context);
  for (unsigned i = 0; i < 3; i++) av_freep(&session->parameters[i].data);
  if (!session->submitted) {
    fprintf(stderr, "FAIL: no decodable access units beginning at an IDR\n");
    result = -1;
  }
  return result < 0 ? -1 : 0;
}

struct producer_args {
  struct session* session;
  const uint8_t* data;
  size_t length;
};

static void* produce_frames(void* opaque) {
  struct producer_args* args = opaque;
  int result = 0;
  while (result == 0 && args->session->submitted < args->session->limit && !cancelled())
    result = play_input(args->session, args->data, args->length);
  pthread_mutex_lock(&producer_mutex);
  producer_result = result;
  producer_done = true;
  pthread_mutex_unlock(&producer_mutex);
  return NULL;
}

static int play_threaded(struct session* session, const uint8_t* data, size_t length) {
  struct producer_args args = { session, data, length };
  pthread_t thread;
  pthread_mutex_lock(&producer_mutex);
  producer_done = producer_cancelled = false;
  producer_result = 0;
  pthread_mutex_unlock(&producer_mutex);
  int error = pthread_create(&thread, NULL, produce_frames, &args);
  if (error) { fprintf(stderr, "FAIL: pthread_create: %s\n", strerror(error)); return -1; }
  int result = 0;
  for (;;) {
    pthread_mutex_lock(&producer_mutex);
    bool done = producer_done;
    pthread_mutex_unlock(&producer_mutex);
    if (done) break;
    if (render_delay_ms) {
      struct timespec delay = { render_delay_ms / 1000, (render_delay_ms % 1000) * 1000000L };
      nanosleep(&delay, NULL);
    }
    if (dispatch_until(monotonic_ns() + 10000000) < 0) {
      pthread_mutex_lock(&producer_mutex);
      producer_cancelled = true;
      pthread_mutex_unlock(&producer_mutex);
      result = -1;
      break;
    }
  }
  error = pthread_join(thread, NULL);
  if (error) { fprintf(stderr, "FAIL: pthread_join: %s\n", strerror(error)); return -1; }
  return result < 0 ? result : producer_result;
}

static int parse_integer(const char* text, int minimum, int maximum, int* value) {
  char* end;
  errno = 0;
  long number = strtol(text, &end, 10);
  if (errno || !*text || *end || number < minimum || number > maximum) return -1;
  *value = (int)number;
  return 0;
}

static uint8_t* load_input(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) { perror("fopen input"); return NULL; }
  if (fseek(file, 0, SEEK_END) < 0) { perror("fseek input"); fclose(file); return NULL; }
  long size = ftell(file);
  if (size <= 0 || (unsigned long)size > MAX_INPUT_BYTES || fseek(file, 0, SEEK_SET) < 0) {
    fprintf(stderr, "FAIL: input must be a nonempty regular Annex-B file of at most %u MiB\n", MAX_INPUT_BYTES / 1024 / 1024);
    fclose(file);
    return NULL;
  }
  uint8_t* data = av_mallocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
  if (!data) { fclose(file); return NULL; }
  size_t count = fread(data, 1, (size_t)size, file);
  int close_result = fclose(file);
  if (count != (size_t)size || close_result) {
    fprintf(stderr, "FAIL: cannot read complete input file\n");
    av_free(data);
    return NULL;
  }
  *length = (size_t)size;
  return data;
}

int main(int argc, char** argv) {
  int width, height, fps, repetitions = 1, seconds = TEST_SECONDS;
  if ((argc < 6 || argc > 8) ||
      (strcmp(argv[2], "h264") && strcmp(argv[2], "h265")) ||
      parse_integer(argv[3], 2, 8192, &width) < 0 ||
      parse_integer(argv[4], 2, 8192, &height) < 0 ||
      parse_integer(argv[5], 1, 240, &fps) < 0 ||
      (argc >= 7 && parse_integer(argv[6], 1, 1000, &repetitions) < 0) ||
      (argc == 8 && parse_integer(argv[7], 1, 600, &seconds) < 0) ||
      (width & 1) || (height & 1)) {
    fprintf(stderr, "Usage: %s input h264|h265 width height fps [repetitions [seconds]]\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  threaded_mode = getenv("MOONLIGHT_TH1520_TEST_THREADED") != NULL;
  const char* delay_option = getenv("MOONLIGHT_TH1520_TEST_RENDER_DELAY_MS");
  if (delay_option && parse_integer(delay_option, 0, 1000, &render_delay_ms) < 0) {
    fprintf(stderr, "MOONLIGHT_TH1520_TEST_RENDER_DELAY_MS must be 0..1000\n");
    return 2;
  }
  const char* padding_option = getenv("MOONLIGHT_TH1520_TEST_TAIL_PADDING");
  const char* burst_option = getenv("MOONLIGHT_TH1520_TEST_BURST");
  test_fullscreen = getenv("MOONLIGHT_TH1520_TEST_FULLSCREEN") != NULL;
  if ((padding_option && parse_integer(padding_option, 0, 1024, &tail_padding_bytes) < 0) ||
      (burst_option && parse_integer(burst_option, 1, 32, &burst_size) < 0)) {
    fprintf(stderr, "TEST_TAIL_PADDING must be 0..1024; TEST_BURST must be 1..32\n");
    return 2;
  }
  printf("THREADING producer=%s renderer=main delay_ms=%d tail_padding=%d burst=%d fullscreen=%d\n",
         threaded_mode ? "worker" : "main", render_delay_ms, tail_padding_bytes, burst_size, test_fullscreen);
  size_t length = 0;
  uint8_t* data = load_input(argv[1], &length);
  if (!data) return 1;
  XSetErrorHandler(x_error_handler);
  Display* probe = XOpenDisplay(NULL);
  if (!probe) {
    fprintf(stderr, "FAIL: cannot open X display (set DISPLAY/XAUTHORITY)\n");
    av_free(data);
    return 1;
  }
  DECODER_RENDERER_CALLBACKS* callbacks = &decoder_callbacks_x11_th1520;
  const char* mode = getenv("MOONLIGHT_TH1520_OUTPUT");
  printf("TEST codec=%s size=%dx%d fps=%d mode=%s repetitions=%d max_frames_per_run=%d\n",
         argv[2], width, height, fps, mode ? mode : "auto", repetitions, fps * seconds);
  int exit_status = 0;
  for (int repetition = 1; repetition <= repetitions; repetition++) {
    long before_fds = count_fds(), before_rss = rss_kib();
    if (before_fds < 0 || before_rss < 0) { exit_status = 1; break; }
    if (handler_count) {
      fprintf(stderr, "FAIL: stale handlers before setup\n");
      exit_status = 1;
      break;
    }
    if (x11_init(false, false) != INIT_EGL) {
      fprintf(stderr, "FAIL: x11_init failed\n");
      exit_status = 1;
      break;
    }
    int format = !strcmp(argv[2], "h265") ? VIDEO_FORMAT_H265 : VIDEO_FORMAT_H264;
    if (callbacks->setup(format, width, height, fps, NULL, DISPLAY_VIEW_ONLY | (test_fullscreen ? DISPLAY_FULLSCREEN : 0)) != 0) {
      fprintf(stderr, "FAIL: production TH1520 setup failed\n");
      exit_status = 1;
      break;
    }
    if (callbacks->start) callbacks->start();
    struct session session = {
      .callbacks = callbacks, .hevc = format == VIDEO_FORMAT_H265,
      .fps = fps, .limit = (unsigned)fps * seconds
    };
    int result = dispatch_until(monotonic_ns() + 50000000);
    if (result == 0 && threaded_mode) result = play_threaded(&session, data, length);
    else while (result == 0 && session.submitted < session.limit)
      result = play_input(&session, data, length);
    if (result == 0) result = dispatch_until(monotonic_ns() + 250000000);
    if (result == 0) {
      const char* screenshot = repetition == repetitions ? getenv("MOONLIGHT_TH1520_SCREENSHOT") : NULL;
      result = check_picture(probe, screenshot);
    }
    if (callbacks->stop) callbacks->stop();
    callbacks->cleanup();
    XSync(probe, False);
    long after_fds = count_fds(), after_rss = rss_kib();
    printf("RUN %d/%d submitted=%u fd_before=%ld fd_after=%ld fd_delta=%+ld rss_before_kib=%ld rss_after_kib=%ld rss_delta_kib=%+ld handlers_after=%zu\n",
           repetition, repetitions, session.submitted, before_fds, after_fds, after_fds - before_fds,
           before_rss, after_rss, after_rss - before_rss, handler_count);
    if (after_fds > before_fds)
      fprintf(stderr, "FAIL: file descriptors leaked across setup/cleanup\n");
    if (result < 0 || after_fds < 0 || after_fds > before_fds || after_rss < 0 || handler_count || loop_failed || x_errors) {
      exit_status = 1;
      break;
    }
  }
  XCloseDisplay(probe);
  av_free(data);
  printf("%s: TH1520 production callback integration\n", exit_status ? "FAIL" : "PASS");
  return exit_status;
}
