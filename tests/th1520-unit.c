/* CPU-only white-box checks for OMX input ownership and size validation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Link with --wrap=avcodec_send_packet; no OMX component is ever opened.
 */
#include "../src/video/ffmpeg.c"
#include <limits.h>

static int send_calls, mock_send_result;
int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
  (void)context; (void)packet;
  send_calls++;
  return mock_send_result;
}
struct access_unit { uint8_t* data; int size; };

static void expect(bool condition, const char* description) {
  printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) exit(1);
}

static struct access_unit load_au(const char* path, enum AVCodecID codec, unsigned index) {
  FILE* file = fopen(path, "rb");
  if (!file) { perror(path); exit(2); }
  expect(fseek(file, 0, SEEK_END) == 0, "seek fixture");
  long size = ftell(file);
  expect(size > 0 && size < 256 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0, "fixture size");
  uint8_t* data = av_mallocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
  expect(data != NULL && fread(data, 1, size, file) == (size_t)size, "read fixture");
  expect(fclose(file) == 0, "close fixture");
  AVCodecParserContext* parser = av_parser_init(codec);
  AVCodecContext* context = avcodec_alloc_context3(NULL);
  expect(parser && context, "allocate public parser");
  struct access_unit result = {0};
  int offset = 0;
  unsigned found = 0;
  while (offset < size) {
    uint8_t* output = NULL;
    int output_size = 0;
    int consumed = av_parser_parse2(parser, context, &output, &output_size,
                                    data + offset, (int)size - offset,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset);
    expect(consumed >= 0 && (consumed || output_size), "parse fixture progress");
    offset += consumed;
    if (output_size && found++ == index) {
      result.data = av_mallocz(output_size + AV_INPUT_BUFFER_PADDING_SIZE);
      expect(result.data != NULL, "copy fixture AU");
      memcpy(result.data, output, output_size);
      result.size = output_size;
      break;
    }
  }
  av_parser_close(parser);
  avcodec_free_context(&context);
  av_free(data);
  expect(result.data != NULL, "find fixture AU");
  return result;
}

static void setup_guard(enum AVCodecID codec, int width, int height) {
  ffmpeg_destroy();
  ffmpeg_decoder = TH1520_OMX;
  th1520_parser_codec = codec;
  th1520_expected_width = width;
  th1520_expected_height = height;
  expect(th1520_reset_parser() == 0, "initialize size guard without hardware");
}

static void test_semiplanar_conversion(void) {
  AVFrame* frame = av_frame_alloc();
  expect(frame != NULL, "allocate NV12 frame");
  frame->format = AV_PIX_FMT_NV12;
  frame->width = frame->height = 4;
  expect(av_frame_get_buffer(frame, 32) == 0, "allocate padded NV12 planes");
  frame->colorspace = AVCOL_SPC_BT709;
  for (int y = 0; y < 4; y++) memset(frame->data[0] + y * frame->linesize[0], 80 + y, 4);
  for (int y = 0; y < 2; y++) {
    unsigned char* uv = frame->data[1] + y * frame->linesize[1];
    uv[0] = 10 + y; uv[1] = 30 + y; uv[2] = 20 + y; uv[3] = 40 + y;
  }
  expect(normalize_software_frame(frame) == 0 && frame->format == AV_PIX_FMT_YUV420P,
         "generic renderer receives planar YUV420, not NV12");
  expect(frame->data[0][frame->linesize[0]] == 81 &&
         frame->data[1][0] == 10 && frame->data[1][1] == 20 &&
         frame->data[2][frame->linesize[2]] == 31 &&
         frame->colorspace == AVCOL_SPC_BT709, "stride, UV order and metadata survive normalization");
  av_frame_free(&frame);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "Usage: %s 1920x1080.h264 1366x768.h264 1280x720.hevc\n", argv[0]);
    return 2;
  }
  struct access_unit hd = load_au(argv[1], AV_CODEC_ID_H264, 0);
  struct access_unit p = load_au(argv[1], AV_CODEC_ID_H264, 1);
  struct access_unit padded = load_au(argv[2], AV_CODEC_ID_H264, 0);
  struct access_unit hevc = load_au(argv[3], AV_CODEC_ID_HEVC, 0);
  uint8_t broken[16 + AV_INPUT_BUFFER_PADDING_SIZE] = {0,0,0,1,0x67,0,0,0,1,0x68,0,0,0,1,0x65,0};

  setup_guard(AV_CODEC_ID_H264, 1920, 1080);
  expect(th1520_validate_size(hd.data, hd.size) == 0 && th1520_coded_height == 1088,
         "visible 1080p with coded 1088 height is accepted");
  expect(th1520_validate_size(p.data, p.size) == 0, "P-frame uses validated dimensions");
  expect(th1520_validate_size(padded.data, padded.size) == AVERROR_INPUT_CHANGED,
         "midstream resolution change is rejected before OMX");
  setup_guard(AV_CODEC_ID_H264, 1366, 768);
  expect(th1520_validate_size(padded.data, padded.size) == 0 && th1520_coded_width == 1376,
         "visible 1366 width is distinguished from coded padding");
  expect(th1520_validate_size(broken, 16) == AVERROR_INVALIDDATA,
         "broken SPS cannot reuse previous dimensions");
  setup_guard(AV_CODEC_ID_HEVC, 1280, 720);
  expect(th1520_validate_size(hevc.data, hevc.size) == 0, "HEVC size validation");

  setup_guard(AV_CODEC_ID_H264, 1920, 1080);
  pkt = av_packet_alloc(); decoder_ctx = avcodec_alloc_context3(NULL);
  expect(pkt && decoder_ctx, "allocate mocked sender");
  mock_send_result = AVERROR(EAGAIN); send_calls = 0;
  expect(send_packet(hd.data, hd.size) == AVERROR(EAGAIN) && packet_pending && send_calls == 1,
         "EAGAIN retains the original validated packet");
  th1520_parser_error = AVERROR_INVALIDDATA;
  mock_send_result = 0;
  expect(send_packet(NULL, 0) == 0 && !packet_pending && send_calls == 2,
         "pending retry neither reparses nor replaces the packet");
  expect(send_packet(NULL, 0) == 0 && send_calls == 2, "idle polling does not send EOS");

  th1520_parser_error = 0;
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode(hd.data, hd.size) == AVERROR(EAGAIN) && !packet_pending,
         "legacy EAGAIN does not wedge all subsequent submissions");
  ffmpeg_destroy();
  test_semiplanar_conversion();
  expect(ffmpeg_init(VIDEO_FORMAT_AV1_MAIN8, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "AV1 rejected before opening hardware");
  expect(ffmpeg_init(VIDEO_FORMAT_H265_MAIN10, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "HEVC Main10 rejected before opening hardware");
  expect(ffmpeg_init(VIDEO_FORMAT_H264_HIGH8_444, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "4:4:4 rejected before opening hardware");
  av_free(hd.data); av_free(p.data); av_free(padded.data); av_free(hevc.data);
  puts("PASS: CPU-only TH1520 ownership/size checks");
  return 0;
}
