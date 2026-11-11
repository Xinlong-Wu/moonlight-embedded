/* CPU-only fixture helper for decoded-frame hash comparisons.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Adds transport zero padding to each parsed H.264 AU, then applies the exact
 * production normalizer. No OMX component or X11 display is opened.
 */
#include "../src/video/ffmpeg.c"
#include <limits.h>
#include <sys/stat.h>

static void require(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static void write_au(FILE* output, const uint8_t* data, int length, int padding, unsigned* count) {
  if (!length) return;
  AVPacket* packet = av_packet_alloc();
  require(packet && length <= INT_MAX - padding && av_new_packet(packet, length + padding) == 0, "allocate AU");
  memcpy(packet->data, data, length);
  memset(packet->data + length, 0, padding);
  require(th1520_normalize_h264_packet(packet) == 0, "normalize complete AU");
  require(fwrite(packet->data, 1, packet->size, output) == (size_t)packet->size, "write normalized AU");
  av_packet_free(&packet);
  (*count)++;
}

int main(int argc, char** argv) {
  if (argc != 4) { fprintf(stderr, "Usage: %s input.h264 output.h264 padding(0..1024)\n", argv[0]); return 2; }
  char* end;
  long padding = strtol(argv[3], &end, 10);
  require(*argv[3] && !*end && padding >= 0 && padding <= 1024, "padding range");
  struct stat in_stat, out_stat;
  require(stat(argv[1], &in_stat) == 0, "input stat");
  require(stat(argv[2], &out_stat) != 0 || in_stat.st_dev != out_stat.st_dev || in_stat.st_ino != out_stat.st_ino,
          "output must not overwrite the input");
  FILE* input = fopen(argv[1], "rb");
  require(input != NULL && fseek(input, 0, SEEK_END) == 0, "open input");
  long length = ftell(input);
  require(length > 0 && length <= 256 * 1024 * 1024 && fseek(input, 0, SEEK_SET) == 0, "input size");
  uint8_t* bytes = av_mallocz(length + AV_INPUT_BUFFER_PADDING_SIZE);
  require(bytes && fread(bytes, 1, length, input) == (size_t)length && fclose(input) == 0, "read input");
  FILE* output = fopen(argv[2], "wb");
  require(output != NULL, "open output");
  AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
  AVCodecContext* context = avcodec_alloc_context3(NULL);
  require(parser && context, "allocate parser");
  int offset = 0;
  unsigned count = 0;
  while (offset < length) {
    uint8_t* data = NULL;
    int size = 0;
    int consumed = av_parser_parse2(parser, context, &data, &size, bytes + offset, (int)length - offset,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset);
    require(consumed >= 0 && (consumed || size), "parser progress");
    offset += consumed;
    write_au(output, data, size, (int)padding, &count);
  }
  uint8_t* data = NULL;
  int size = 0;
  require(av_parser_parse2(parser, context, &data, &size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset) >= 0,
          "flush parser");
  write_au(output, data, size, (int)padding, &count);
  require(fclose(output) == 0 && count > 0, "finish output");
  av_parser_close(parser);
  avcodec_free_context(&context);
  av_free(bytes);
  printf("PASS: normalized %u H.264 access units after adding %ld zero bytes per AU\n", count, padding);
  return 0;
}
