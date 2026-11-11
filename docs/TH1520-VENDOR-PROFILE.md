# TH1520 vendor-send profiling and H.264 filler optimization

## Outcome (2026-11-11)

The earlier live 1080p60 backlog was traced to the Hantro SDK's **software handling of H.264 filler-data NALs**, not the encoded-packet copy and not the measured ASIC decode wait. The client now omits only syntax-valid, non-reference filler NALs before handing an AU to OMX.

A normal build, with no diagnostic preload library and with the private FFmpeg restored to its original lifetime-only patch, completed a bounded 60-second 1920×1080/60 FPS/20 Mbps view-only host test with:

- No `Video decode unit queue overflow`, `BYTE_STREAM` or input-gap messages.
- `decoded=3543 presented=3524 replaced=18`; a queued/end-of-session frame accounts for the remaining difference. This is not a claim that every input picture was presented or that end-to-end latency was measured.
- Submission average **0.572 ms**, maximum **9.788 ms**; send API average **0.136 ms**.
- Maximum observed core queue age **20.955 ms** (queue age alone is not end-to-end latency).
- 3504 filler NALs / 108,368,545 filler bytes removed locally. Network traffic and picture timestamps were not changed.
- RSS samples after warmup of 66,676, 66,684 and 66,688 KiB at approximately 15, 30 and 45 seconds. This short observation is not a long-session leak certification.

This supersedes the remaining-throughput conclusion in the [earlier stream follow-up](<TH1520-STREAM-FIX.md>) for the specific tested H.264 workload. Longer gameplay, input, subjective A/V synchronization, other hosts and 4K still require validation.

## What “the send call chain” actually contains

In the pinned RevyOS FFmpeg, `avcodec_send_packet()` can immediately call `decode_receive_frame_internal()`. That enters the OMX wrapper's receive/feed loop, which can wait for an input buffer before returning. Thus the public send duration does not identify a memcpy bottleneck by itself.

```text
Moonlight Decode Unit
  -> avcodec_send_packet
     -> FFmpeg receive/feed loop
        -> wait for a free OMX input buffer
        -> copy the encoded AU
        -> OMX_EmptyThisBuffer
           -> SDK component worker
              -> H264DecDecode
                 -> software NAL processing, or
                 -> H264RunAsic / DWLWaitCmdBufReady
        -> output allocation/wrapping and callback refills
```

## Measurement and controls

### 1. Native wrapper-stage aggregates

A profiling-only incremental [FFmpeg patch](<patches/revyos-ffmpeg-omx-profile.patch>) adds aggregate counters without changing waits, branches, buffer counts or hardware parameters. The initial live measurement showed:

| Stage | Calls | Total / average |
| --- | ---: | --- |
| Entire OMX receive/feed loop | 853 | 10,102,397 µs total |
| Input-buffer acquisition/wait | 427 | 10,027,288 µs total; **23,483 µs average** |
| Encoded input memcpy | 427 | 81 µs average |
| `OMX_EmptyThisBuffer` | 427 | 10 µs average |
| Port-definition query | 15,028 | 0.316 µs average |
| Output `ff_get_buffer` | 422 | 23 µs average |
| Output wrapping | 422 | 11 µs average |

Approximately 99% of receive-loop wall time in that run was spent waiting for input buffers to be returned. Stage averages have different denominators, and inclusive/nested durations must not be added together.

The component had four allocated input buffers and ten output buffers. Increasing these counts would only hide a slower consumer temporarily, so the optimization did not resize them.

### 2. SDK-side native timing and configuration checks

Local diagnostic interposers forwarded arguments to the original exported SDK functions and recorded only numeric call counts, durations, lengths and NAL types. Their ABIs were checked against this installed SDK. They were not binary patches, did not save picture data, and are **not** loaded by the normal client or required for the optimization.

The live stream and a generated High-profile, CABAC, single-slice CBR clip shared the relevant SPS configuration: profile 100, level 42, 8-bit 4:2:0, one reference frame, progressive frames, POC type 2, zero reorder frames and max DPB 1. Ordinary local CBR/VBR clips decoded quickly, ruling out a generic inability to handle that profile, slice count or encoded rate.

Observed native timings narrowed the difference further:

- ASIC command-buffer wait averaged about **7.6–8 ms per picture** for both workloads.
- NAL extraction and ordinary copies were small.
- In the problematic stream, the SDK's return-code-1 calls associated with NAL **type 12** averaged about **23.55 ms**, often processing tens of KiB of filler rather than a picture.
- A generated, static CBR color-bar clip reproduced the same filler path at about **25.27 ms** per filler NAL. A moving CBR test clip did not adequately exercise that path.

A debugger stack snapshot was used only to identify functions. Pausing threads causes artificial packet loss and timing spikes; that snapshot was not used as a throughput benchmark.

### 3. Why skipping this NAL is appropriate

H.264 filler-data NALs carry rate-padding, not decoded picture/reference information. The reference software decoder also ignores `H264_NAL_FILLER_DATA` ([pinned source](https://github.com/revyos/ffmpeg/blob/f86f86b3f7087b4e809a079371c4bc513d3d50f8/libavcodec/h264dec.c#L720-L726)).

The client is deliberately stricter than dropping anything labeled type 12:

- Header must be exactly `0x0c`: type 12, forbidden bit clear, `nal_ref_idc=0`.
- Payload must be `0xff` repeated zero or more times, followed by `0x80` (`rbsp_trailing_bits`) and only Annex-B zero tail bytes.
- The full AU's start-code/header framing is checked before mutation. Legal filler can occur at the beginning, middle or end.
- All retained NALs, including SPS/PPS/SEI and slices/EPBs, remain byte-identical except for the pre-existing excessive-zero-tail policy. Malformed or non-reference-invalid type-12 payloads are not blindly removed.
- If a valid AU is entirely filler, it is consumed as an accepted no-op. No zero-length packet is sent to FFmpeg, no EOS is generated, and no pending packet is left behind.
- KEY/PTS and EAGAIN retry ownership are retained; normalization and removal statistics are not repeated on a pending retry.
- HEVC and other video backends remain unchanged.

This is local decoder-input adaptation. It does not alter RTP/FEC transmission, change encoded picture syntax, force reference-frame counts, or change the network bitrate requested from the host.

## Before/after evidence

| Workload | Before | After |
| --- | --- | --- |
| Static CBR, 1080p60, 120 generated AUs | Submit average 30.935 ms; input wait average 30.464 ms | Submit average 0.502 ms; input wait average 0.958 µs |
| Static CBR decoded pixels | Original clip reference | Filtered/zero-padded clip has exactly identical software-decoded frame MD5 output |
| Live 1080p60/20 Mbps, instrumented 20-second test | Earlier runs repeatedly overflowed | 1138 decoded / 1137 presented; no input gap or overflow; send average 0.107 ms |
| Live 1080p60/20 Mbps, **normal** 60-second process limit | Not used as an old/new same-scene FPS benchmark | 3543 decoded / 3524 presented; no input gap or overflow; send average 0.136 ms |
| Twenty same-process full-screen static-CBR/padded restarts | New regression | All passed; fd count returned to 4 and all event registrations were removed |

Live desktop content and session durations were not controlled against old recordings, so old/new presentation counters alone are not a rigorous FPS benchmark. The generated static-CBR reproduction, native hotspot measurements, identical frame hashes, filler-removal counts and final uninstrumented run provide independent checks of the causal change.

## Reproducing the generated workload

Build the optional test targets using `-DBUILD_TH1520_TESTS=ON`. Use generated content only:

```sh
ffmpeg -y -f lavfi -i smptebars=size=1920x1080:rate=60 -frames:v 120 \
  -c:v libx264 -threads 1 -preset veryfast -profile:v high -tune zerolatency \
  -b:v 15M -minrate 15M -maxrate 15M -bufsize 15M \
  -x264-params cabac=1:8x8dct=1:sliced-threads=0:slices=1:repeat-headers=1:nal-hrd=cbr:filler=1 \
  -f h264 /tmp/static-cbr.h264
./build/th1520-normalize-fixture /tmp/static-cbr.h264 /tmp/static-filtered.h264 512
ffmpeg -y -c:v h264 -i /tmp/static-cbr.h264 -f framemd5 /tmp/original.md5
ffmpeg -y -c:v h264 -i /tmp/static-filtered.h264 -f framemd5 /tmp/filtered.md5
cmp /tmp/original.md5 /tmp/filtered.md5
DISPLAY=:0 LD_LIBRARY_PATH="$PWD/build/ffmpeg-fixed/lib" \
  MOONLIGHT_TH1520_OUTPUT=drm MOONLIGHT_TH1520_TEST_THREADED=1 \
  MOONLIGHT_TH1520_TEST_TAIL_PADDING=512 \
  ./build/th1520-smoke /tmp/static-cbr.h264 h264 1920 1080 60 1 2
```

The slower baseline client was commit `5da688a`; use a separate checkout when comparing it, not an in-place checkout that discards uncommitted work. The filler-unit tests cover placement, malformed payloads, filler-only no-ops, metadata/pending behavior and unchanged HEVC/non-TH1520 paths. Reference frame hashes verify actual CABAC picture preservation, not just byte-count expectations.

## Optional SDK wrapper profiler

The default private-library build script still applies **only the lifetime repair**. No SDK profiler, native ABI shim, custom hardware parameter or larger queue is required by the delivered client optimization.

For additional diagnostics, first build the private lifetime-fixed source normally. Ensure no streams/tests are using the private prefix before replacing its libraries. The optional profile patch applies on top of that source:

```sh
git -C build/revyos-ffmpeg apply --check "$PWD/docs/patches/revyos-ffmpeg-omx-profile.patch"
git -C build/revyos-ffmpeg apply "$PWD/docs/patches/revyos-ffmpeg-omx-profile.patch"
make -C build/ffmpeg-obj -j4
make -C build/ffmpeg-obj install
# Run one controlled test, then stop it before reverting/reinstalling.
git -C build/revyos-ffmpeg apply --reverse "$PWD/docs/patches/revyos-ffmpeg-omx-profile.patch"
make -C build/ffmpeg-obj -j4
make -C build/ffmpeg-obj install
```

It reports aggregate input waits/copies/submission, control waits, port queries, output allocation/wrapping, refills, loop and buffer/window counts at cleanup. The separate native SDK interposers were local workbench diagnostics for the installed binary, not portable production components.

## Validation boundary and local artifacts

Actual-host tests were view-only with `-nosops`, and saved diagnostic logs only. Interactive input, subjective audio/A/V synchronization, long real gameplay, 4K, network-failure recovery and other SDK binaries are not certified by these results. HEVC real-stream tail/filler handling was not changed. Eighteen decoded frames were replaced during the final one-minute run; do not describe it as zero frame replacement or zero end-to-end latency.

Local evidence (build outputs are intentionally ignored by Git):

- [Coarse OMX stage baseline](<../build/vendor-profile/live-stages-baseline.log>).
- [Static-CBR slow-path reproduction](<../build/vendor-profile/static-cbr-sdk.log>).
- [Live filler-NAL classification](<../build/vendor-profile/live-nal-class.log>).
- [Filtered fixture normalization](<../build/vendor-profile/static-filter-normalize.log>).
- [Normal uninstrumented one-minute run](<../build/vendor-profile/live-normal-60s.log>).
- [RSS samples](<../build/vendor-profile/live-normal-memory.log>).
- [Twenty restarts](<../build/vendor-profile/filler-restarts-20.log>).

The final private FFmpeg source was restored to a diff byte-identical to the original lifetime patch before the normal test. Temporary profiler/preload libraries were not used for that test. The new client marker includes `h264_filler=skip-valid`, and exit statistics show `filler_nals_removed`, `filler_bytes_removed` and `filler_only_noops`.
