/* CPU-only white-box checks for OMX input ownership and size validation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Link with --wrap=avcodec_send_packet; no OMX component is ever opened.
 */
#include "../src/video/ffmpeg.c"
#include <limits.h>

static int send_calls, mock_send_result;
static AVPacket* captured_packet;
static bool captured_padding_zero;
int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
  (void)context;
  send_calls++;
  av_packet_unref(captured_packet);
  if (!packet || av_packet_ref(captured_packet, packet) < 0) abort();
  captured_padding_zero = true;
  for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++)
    if (packet->data[packet->size + i] != 0) captured_padding_zero = false;
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

// Use a real software receive context with a wrapped sender: receive returns
// EAGAIN because no packet reaches it. No OMX component is opened by these tests.
static void setup_mock_sender(enum AVCodecID codec, int width, int height, bool th1520) {
  if (th1520) setup_guard(codec, width, height);
  else { ffmpeg_destroy(); ffmpeg_decoder = SOFTWARE; }
  decoder = avcodec_find_decoder_by_name(codec == AV_CODEC_ID_HEVC ? "hevc" : "h264");
  expect(decoder != NULL, "find CPU-only decoder for mocked receive");
  decoder_ctx = avcodec_alloc_context3(decoder);
  expect(decoder_ctx != NULL, "allocate mocked receive context");
  decoder_ctx->width = width; decoder_ctx->height = height;
  decoder_ctx->thread_count = 1;
  decoder_ctx->time_base = (AVRational) { 1, AV_TIME_BASE };
  decoder_ctx->pkt_timebase = decoder_ctx->time_base;
  expect(avcodec_open2(decoder_ctx, decoder, NULL) == 0, "open software-only receive context");
  pkt = av_packet_alloc();
  dec_frames_cnt = 1;
  dec_frames = calloc(1, sizeof(*dec_frames));
  expect(pkt && dec_frames, "allocate mocked sender frame ring");
  dec_frames[0] = av_frame_alloc();
  expect(dec_frames[0] != NULL, "allocate receive frame");
  send_calls = 0; mock_send_result = 0;
  av_packet_unref(captured_packet);
}

static struct access_unit append_bytes(struct access_unit input, const uint8_t* suffix, int length) {
  struct access_unit result = { .size = input.size + length };
  result.data = av_mallocz((size_t)result.size + AV_INPUT_BUFFER_PADDING_SIZE);
  expect(result.data != NULL, "allocate extended AU");
  memcpy(result.data, input.data, input.size);
  if (suffix) memcpy(result.data + input.size, suffix, length);
  return result;
}

static int canonical_h264_size(const uint8_t* data, int size) {
  int end = size;
  while (end > 0 && data[end - 1] == 0) end--;
  return end + (size - end > 2 ? 2 : size - end);
}

static void test_tail_framing(void) {
  const uint8_t cabac[] = {0,0,0,1,0x65,0x80,0,0};
  const uint8_t escaped[] = {0,0,1,0x65,0x80,0,0,3,0};
  const uint8_t escaped_word[] = {0,0,1,0x65,0x80,0,0,3,0,0};
  const uint8_t internal[] = {0,0,0,1,0x67,0x80,0,0,1,0x68,0x80,0,0,1,0x65,0,0,3,0,0x42,0x80};
  const uint8_t all_zero[] = {0,0,0,0,0,0,0,0};
  const uint8_t avcc[] = {0,0,0,4,0x65,0x80,0x20,0x80};
  const uint8_t dangling_epb[] = {0,0,1,0x65,0x80,0,0,3};
  const uint8_t dangling_prefix[] = {0,0,1,0x65,0x80,0,0,1};
  const uint8_t forbidden[] = {0,0,1,0xe5,0x80};
  struct framing_case { const uint8_t* bytes; int size; bool valid; } cases[] = {
    {cabac, sizeof(cabac), true}, {escaped, sizeof(escaped), true},
    {escaped_word, sizeof(escaped_word), true}, {internal, sizeof(internal), true},
    {all_zero, sizeof(all_zero), false}, {avcc, sizeof(avcc), false},
    {dangling_epb, sizeof(dangling_epb), false},
    {dangling_prefix, sizeof(dangling_prefix), false}, {forbidden, sizeof(forbidden), false},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    AVPacket* sample = av_packet_alloc();
    expect(sample && av_new_packet(sample, cases[i].size) == 0, "allocate framing case");
    memcpy(sample->data, cases[i].bytes, cases[i].size);
    int result = th1520_normalize_h264_packet(sample);
    expect((result == 0) == cases[i].valid, "framing validity classification");
    if (cases[i].valid)
      expect(sample->size == cases[i].size && !memcmp(sample->data, cases[i].bytes, sample->size),
             "CABAC, escaped final zero and internal NAL bytes remain identical");
    av_packet_free(&sample);
  }
  // Synthetic suffix framing checks, not a claim of complete slice validation.
  AVPacket* sample = av_packet_alloc();
  expect(sample && av_new_packet(sample, sizeof(escaped) + 1024) == 0, "allocate EPB tail case");
  memcpy(sample->data, escaped, sizeof(escaped));
  memset(sample->data + sizeof(escaped), 0, 1024);
  expect(th1520_normalize_h264_packet(sample) == 0 && sample->size == (int)sizeof(escaped) + 1 &&
         !memcmp(sample->data, escaped, sizeof(escaped)), "long tail preserves terminal 00 00 03 00");
  av_packet_free(&sample);
}

static void expect_filtered(const uint8_t* input, int input_size, const uint8_t* expected,
                            int expected_size, uint64_t nal_count, uint64_t byte_count,
                            const char* description) {
  AVPacket* sample = av_packet_alloc();
  expect(sample && av_new_packet(sample, input_size) == 0, "allocate filler framing case");
  memcpy(sample->data, input, input_size);
  sample->flags = AV_PKT_FLAG_KEY; sample->pts = 12345678;
  uint64_t before_nals = filler_nal_count, before_bytes = filler_byte_count;
  expect(th1520_normalize_h264_packet(sample) == 0 && sample->size == expected_size &&
         (expected_size == 0 || !memcmp(sample->data, expected, expected_size)) &&
         sample->flags == AV_PKT_FLAG_KEY && sample->pts == 12345678 &&
         filler_nal_count - before_nals == nal_count && filler_byte_count - before_bytes == byte_count,
         description);
  bool padding_zero = true;
  for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++)
    if (sample->data[sample->size + i] != 0) padding_zero = false;
  expect(padding_zero, "compacted/filler-only packet retains FFmpeg zero padding");
  av_packet_free(&sample);
}

static void test_filler_framing(void) {
  const uint8_t slice[] = {0,0,1,0x65,0,0,3,0,0x42,0x80,0,0};
  const uint8_t filler3[] = {0,0,1,12,0xff,0xff,0x80};
  const uint8_t filler4[] = {0,0,0,1,12,0x80};
  const uint8_t leading[] = {0,0};
  struct access_unit prefix = { .data = (uint8_t*)leading, .size = sizeof(leading) };
  struct access_unit head = append_bytes(prefix, filler4, sizeof(filler4));
  struct access_unit head_input = append_bytes(head, slice, sizeof(slice));
  struct access_unit head_expected = append_bytes(prefix, slice, sizeof(slice));
  expect_filtered(head_input.data, head_input.size, head_expected.data, head_expected.size,
                  1, sizeof(filler4), "head filler removal preserves leading zeros and final escaped/cabac bytes");

  const uint8_t config[] = {0,0,0,1,0x67,0x80,0,0,1,0x68,0x80};
  struct access_unit config_au = { .data = (uint8_t*)config, .size = sizeof(config) };
  struct access_unit middle = append_bytes(config_au, filler3, sizeof(filler3));
  struct access_unit middle_input = append_bytes(middle, slice, sizeof(slice));
  struct access_unit middle_expected = append_bytes(config_au, slice, sizeof(slice));
  expect_filtered(middle_input.data, middle_input.size, middle_expected.data, middle_expected.size,
                  1, sizeof(filler3), "middle filler removal preserves config, startcodes and internal EPB");

  struct access_unit slice_au = { .data = (uint8_t*)slice, .size = sizeof(slice) };
  struct access_unit tail = append_bytes(slice_au, filler4, sizeof(filler4));
  struct access_unit tail_input = append_bytes(tail, NULL, 512);
  expect_filtered(tail_input.data, tail_input.size, slice, sizeof(slice),
                  1, sizeof(filler4) + 512, "tail filler and its transport zeros are removed without touching slice zero word");

  struct access_unit filler_au = { .data = (uint8_t*)filler3, .size = sizeof(filler3) };
  struct access_unit all_filler = append_bytes(filler_au, filler4, sizeof(filler4));
  expect_filtered(all_filler.data, all_filler.size, NULL, 0, 2, sizeof(filler3) + sizeof(filler4),
                  "all syntax-valid fillers compact to an intentional empty no-op");

  const uint8_t nri[] = {0,0,1,0x2c,0xff,0x80};
  const uint8_t arbitrary[] = {0,0,1,12,0xff,0x42,0x80};
  const uint8_t missing_stop[] = {0,0,1,12,0xff,0xff};
  const uint8_t after_stop[] = {0,0,0,1,12,0xff,0x80,1};
  const uint8_t escaped[] = {0,0,1,12,0xff,0,0,3,0,0x80};
  struct { const uint8_t* bytes; int size; } invalid_fillers[] = {
    {nri, sizeof(nri)}, {arbitrary, sizeof(arbitrary)}, {missing_stop, sizeof(missing_stop)},
    {after_stop, sizeof(after_stop)}, {escaped, sizeof(escaped)},
  };
  for (unsigned i = 0; i < sizeof(invalid_fillers) / sizeof(invalid_fillers[0]); i++)
    expect_filtered(invalid_fillers[i].bytes, invalid_fillers[i].size,
                    invalid_fillers[i].bytes, invalid_fillers[i].size, 0, 0,
                    "malformed or reference type12 NAL is not blindly deleted");

  const uint8_t bad_late[] = {0,0,1,12,0xff,0x80,0,0,1,0xe5,0x80};
  AVPacket* invalid = av_packet_alloc();
  expect(invalid && av_new_packet(invalid, sizeof(bad_late)) == 0, "allocate late-invalid-header case");
  memcpy(invalid->data, bad_late, sizeof(bad_late));
  uint64_t before_nals = filler_nal_count, before_bytes = filler_byte_count;
  expect(th1520_normalize_h264_packet(invalid) == AVERROR_INVALIDDATA &&
         invalid->size == sizeof(bad_late) && !memcmp(invalid->data, bad_late, sizeof(bad_late)) &&
         filler_nal_count == before_nals && filler_byte_count == before_bytes,
         "a late invalid header aborts before any compaction or stats update");
  av_packet_free(&invalid);
  av_free(head.data); av_free(head_input.data); av_free(head_expected.data);
  av_free(middle.data); av_free(middle_input.data); av_free(middle_expected.data);
  av_free(tail.data); av_free(tail_input.data); av_free(all_filler.data);
}

static void test_filler_sender(struct access_unit hd) {
  const uint8_t filler[] = {0,0,0,1,12,0xff,0xff,0x80,0,0};
  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, true);
  AVFrame* output = NULL;
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode_frame_ex((unsigned char*)filler, sizeof(filler), true, 55, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         !packet_pending && send_calls == 0 && pkt->size == 0 && filler_noop_count == 1 &&
         filler_nal_count == 1 && filler_byte_count == sizeof(filler),
         "filler-only is accepted before dimensions exist without sending a zero packet or EOS");
  expect(ffmpeg_decode_frame_ex(NULL, 0, false, 0, &output) == 0 && !packet_pending && send_calls == 0 &&
         filler_noop_count == 1, "poll after filler-only neither repeats normalization nor sends EOS");
  expect(ffmpeg_decode((unsigned char*)filler, sizeof(filler)) == 0 && send_calls == 0 && filler_noop_count == 2,
         "legacy filler-only consumption is also a no-op");

  struct access_unit input = append_bytes(hd, filler, sizeof(filler));
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode_frame_ex(input.data, input.size, true, 98765, &output) == FFMPEG_DECODE_PACKET_PENDING &&
         packet_pending && send_calls == 1 && captured_packet->size == canonical_h264_size(hd.data, hd.size) &&
         !memcmp(captured_packet->data, hd.data, captured_packet->size) && captured_padding_zero &&
         captured_packet->flags == AV_PKT_FLAG_KEY && captured_packet->pts == 98765,
         "mixed AU sends only retained picture/config bytes with original KEY and PTS");
  uint64_t normalized_nals = filler_nal_count, normalized_bytes = filler_byte_count;
  uint8_t* owned_data = captured_packet->data;
  int owned_size = captured_packet->size;
  expect(ffmpeg_decode_frame_ex((unsigned char*)filler, sizeof(filler), false, 0, &output) == AVERROR(EAGAIN) &&
         send_calls == 1 && filler_noop_count == 2 && filler_nal_count == normalized_nals,
         "filler-only cannot bypass or replace an older pending picture packet");
  memset(input.data, 0xa5, input.size);
  th1520_parser_error = AVERROR_INVALIDDATA;
  mock_send_result = 0;
  expect(ffmpeg_decode_frame_ex(NULL, 0, false, 1, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         !packet_pending && send_calls == 2 && captured_packet->data == owned_data && captured_packet->size == owned_size &&
         captured_packet->flags == AV_PKT_FLAG_KEY && captured_packet->pts == 98765 && captured_padding_zero &&
         filler_nal_count == normalized_nals && filler_byte_count == normalized_bytes && filler_noop_count == 2,
         "EAGAIN retry does not refilter/count and retains original filler-free bytes and metadata");
  av_free(input.data);

  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, true);
  const uint8_t invalid_filler[] = {0,0,1,12,0xff,0x42,0x80};
  input = append_bytes(hd, invalid_filler, sizeof(invalid_filler));
  expect(ffmpeg_decode_frame_ex(input.data, input.size, false, 777, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->size == input.size && !memcmp(captured_packet->data, input.data, input.size) &&
         filler_nal_count == 0 && filler_byte_count == 0,
         "actual sender retains a malformed type12 payload instead of concealing it");
  av_free(input.data);

  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, false);
  expect(ffmpeg_decode_frame_ex((unsigned char*)filler, sizeof(filler), true, 666, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         send_calls == 1 && captured_packet->size == sizeof(filler) &&
         !memcmp(captured_packet->data, filler, sizeof(filler)) &&
         captured_packet->flags == AV_PKT_FLAG_KEY && captured_packet->pts == 666 &&
         filler_nal_count == 0 && filler_noop_count == 0,
         "non-TH1520 sender forwards legal filler-only bytes unchanged");
  ffmpeg_destroy();
}

static void test_packet_normalization(struct access_unit hd) {
  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, true);
  uint8_t* input = av_mallocz((size_t)hd.size + 1024 + AV_INPUT_BUFFER_PADDING_SIZE);
  expect(input != NULL, "allocate FEC padding test AU");
  memcpy(input, hd.data, hd.size);
  for (int zeros = 0; zeros <= 1024; zeros++) {
    AVFrame* output = NULL;
    int64_t pts = INT64_C(1000000) + zeros;
    int result = ffmpeg_decode_frame_ex(input, hd.size + zeros, true, pts, &output);
    int expected = canonical_h264_size(input, hd.size + zeros);
    if (result != FFMPEG_DECODE_PACKET_ACCEPTED || output || captured_packet->size != expected ||
        memcmp(captured_packet->data, input, expected) || !captured_padding_zero ||
        captured_packet->flags != AV_PKT_FLAG_KEY || captured_packet->pts != pts ||
        captured_packet->dts != AV_NOPTS_VALUE) {
      fprintf(stderr, "FAIL: tail/meta preservation at %d FEC zero bytes\n", zeros);
      exit(1);
    }
  }
  expect(send_calls == 1025, "0..1024 FEC tail lengths normalize once and retain KEY/PTS/padding");
  const uint8_t zero_au[] = {0,0,0,0,0,0,0,0};
  const uint8_t invalid_au[] = {0,0,0,4,0x65,0x80,0x20,0x80};
  AVFrame* invalid_output = NULL;
  expect(ffmpeg_decode_frame_ex((unsigned char*)zero_au, sizeof(zero_au), true, 1, &invalid_output) == AVERROR_INVALIDDATA &&
         !packet_pending && send_calls == 1025,
         "all-zero AU is rejected without sending anything to OMX");
  expect(ffmpeg_decode_frame_ex((unsigned char*)invalid_au, sizeof(invalid_au), true, 2, &invalid_output) == AVERROR_INVALIDDATA &&
         !packet_pending && send_calls == 1025,
         "non-Annex-B AU is rejected without sending anything to OMX");

  // A valid SEI user_data_unregistered payload contains an escaped zero. The
  // complete sender must preserve this internal NAL, not only the helper test.
  const uint8_t sei[] = {0,0,1,6,5,16,0,0,3,0,1,2,3,4,5,6,7,8,9,10,11,12,13,0x80};
  struct access_unit extended = append_bytes(hd, sei, sizeof(sei));
  struct access_unit padded = append_bytes(extended, NULL, 512);
  AVFrame* output = NULL;
  expect(ffmpeg_decode_frame_ex(padded.data, padded.size, false, 2345678, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->size == extended.size + 2 && !memcmp(captured_packet->data, extended.data, extended.size) &&
         captured_packet->flags == 0 && captured_packet->pts == 2345678 && captured_padding_zero,
         "actual sender preserves internal start codes and escaped SEI bytes");
  av_free(input); av_free(extended.data); av_free(padded.data);
}

static void test_pending_metadata(struct access_unit hd, struct access_unit p) {
  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, true);
  struct access_unit input = append_bytes(hd, NULL, 512);
  AVFrame* output = NULL;
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode_frame_ex(input.data, input.size, true, 987654321, &output) == FFMPEG_DECODE_PACKET_PENDING &&
         packet_pending && send_calls == 1, "EAGAIN retains the original normalized packet");
  uint8_t* owned_data = captured_packet->data;
  int owned_size = captured_packet->size;
  memset(input.data, 0xa5, input.size);
  expect(ffmpeg_decode_frame_ex(p.data, p.size, false, 1, &output) == AVERROR(EAGAIN) && send_calls == 1,
         "new input cannot replace a pending packet");
  th1520_parser_error = AVERROR_INVALIDDATA;
  mock_send_result = 0;
  expect(ffmpeg_decode_frame_ex(NULL, 0, false, -123, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         !packet_pending && send_calls == 2 && captured_packet->data == owned_data &&
         captured_packet->size == owned_size && !memcmp(captured_packet->data, hd.data, hd.size) &&
         captured_packet->flags == AV_PKT_FLAG_KEY && captured_packet->pts == 987654321 && captured_padding_zero,
         "retry keeps owned bytes, KEY and PTS despite changed caller metadata and buffer");
  expect(ffmpeg_decode_frame_ex(NULL, 0, true, 777, &output) == 0 && send_calls == 2,
         "idle poll neither sends EOS nor changes captured metadata");
  th1520_parser_error = 0;
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode_frame_ex(p.data, p.size, false, 100, &output) == FFMPEG_DECODE_PACKET_PENDING,
         "retain a non-key packet");
  mock_send_result = 0;
  expect(ffmpeg_decode_frame_ex(NULL, 0, true, 200, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->flags == 0 && captured_packet->pts == 100,
         "retry cannot add a key flag or replace the original PTS");
  av_free(input.data);
}

static void test_legacy_and_other_codecs(struct access_unit hd, struct access_unit hevc) {
  AVFrame* output = NULL;
  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, true);
  mock_send_result = AVERROR(EAGAIN);
  expect(ffmpeg_decode(hd.data, hd.size) == AVERROR(EAGAIN) && !packet_pending &&
         captured_packet->flags == 0 && captured_packet->pts == AV_NOPTS_VALUE,
         "legacy EAGAIN drops saved packet and retains legacy metadata defaults");
  mock_send_result = 0;
  expect(ffmpeg_decode_frame(hd.data, hd.size, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->flags == 0 && captured_packet->pts == AV_NOPTS_VALUE,
         "old decode_frame wrapper supplies false and unknown PTS");
  expect(ffmpeg_decode_frame_ex(hd.data, hd.size, true, 1, NULL) == AVERROR(EINVAL),
         "extended API rejects a NULL output pointer");

  setup_mock_sender(AV_CODEC_ID_HEVC, 1280, 720, true);
  struct access_unit padded = append_bytes(hevc, NULL, 512);
  expect(ffmpeg_decode_frame_ex(padded.data, padded.size, true, 3456789, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->size == padded.size && !memcmp(captured_packet->data, padded.data, padded.size) &&
         captured_packet->flags == AV_PKT_FLAG_KEY && captured_packet->pts == 3456789,
         "TH1520 HEVC bytes and tail are untouched while metadata is forwarded");
  av_free(padded.data);
  const uint8_t hevc_filler[] = {0,0,0,1,0x4c,1,0xff,0xff,0x80,0,0};
  padded = append_bytes(hevc, hevc_filler, sizeof(hevc_filler));
  expect(ffmpeg_decode_frame_ex(padded.data, padded.size, false, 4567890, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->size == padded.size && !memcmp(captured_packet->data, padded.data, padded.size) &&
         filler_nal_count == 0 && filler_noop_count == 0,
         "TH1520 HEVC filler NALs are not modified by the H264 workaround");
  av_free(padded.data);

  setup_mock_sender(AV_CODEC_ID_H264, 1920, 1080, false);
  padded = append_bytes(hd, NULL, 512);
  expect(ffmpeg_decode_frame(padded.data, padded.size, &output) == FFMPEG_DECODE_PACKET_ACCEPTED &&
         captured_packet->size == padded.size && !memcmp(captured_packet->data, padded.data, padded.size) &&
         captured_packet->flags == 0 && captured_packet->pts == AV_NOPTS_VALUE,
         "non-TH1520 backend and legacy packet bytes are untouched");
  av_free(padded.data);
  ffmpeg_destroy();
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
  captured_packet = av_packet_alloc();
  expect(captured_packet != NULL, "allocate sender capture");
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

  test_tail_framing();
  test_filler_framing();
  test_filler_sender(hd);
  test_packet_normalization(hd);
  test_pending_metadata(hd, p);
  test_legacy_and_other_codecs(hd, hevc);
  test_semiplanar_conversion();
  expect(ffmpeg_init(VIDEO_FORMAT_AV1_MAIN8, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "AV1 rejected before opening hardware");
  expect(ffmpeg_init(VIDEO_FORMAT_H265_MAIN10, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "HEVC Main10 rejected before opening hardware");
  expect(ffmpeg_init(VIDEO_FORMAT_H264_HIGH8_444, 1920, 1080, TH1520_ACCELERATION, 2, 1) < 0,
         "4:4:4 rejected before opening hardware");
  av_free(hd.data); av_free(p.data); av_free(padded.data); av_free(hevc.data);
  av_packet_free(&captured_packet);
  puts("PASS: CPU-only TH1520 ownership/size checks");
  return 0;
}
