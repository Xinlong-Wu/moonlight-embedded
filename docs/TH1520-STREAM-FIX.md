# TH1520 real-stream follow-up (2026-11-11)

**Historical phase:** this follow-up records the tail/control-lock fixes before the later filler-data optimization. The queue-overflow measurements below were real at that stage. They are superseded for the tested H.264 workload by [vendor-send profiling and filler removal](<TH1520-VENDOR-PROFILE.md>), whose normal one-minute 1080p60 run had no queue overflow. The old observations remain here for traceability, not as the current throughput verdict.

## Implemented fixes

### H.264 access-unit tail normalization

Moonlight-common retains H.264/HEVC FEC tail padding. With the installed Hantro SDK, adding 512 zero bytes to each otherwise valid H.264 AU reproduced one `ERROR: BYTE_STREAM` message per input AU. Read-only SDK analysis located this diagnostic in the H.264 NAL/Annex-B extraction path; it is not an EGL error and does not by itself mean every frame failed to decode.

The TH1520 H.264 sender now normalizes only the **final long, all-zero tail** of its owned packet. It retains up to two zeros, preserving possible CABAC zero-word/final EBSP bytes and never truncating a terminal `00 00 03 00` into `00 00 03`. Internal start codes, emulation-prevention bytes, SPS/PPS and picture bytes are not rewritten. This is not a complete malicious-bitstream validator. HEVC and non-TH1520 senders are unchanged.

Normalization and size validation run once on new input. An EAGAIN retry retains the original owned bytes, effective length, key flag and timestamp. The new `ffmpeg_decode_frame_ex()` forwards IDR as `AV_PKT_FLAG_KEY` and presentation time in microseconds; legacy interfaces remain available.

### Independent presentation control

Normal presentation no longer takes the mutex held across vendor FFmpeg/OMX send/receive calls. A separate short state mutex protects stop/restart/failure flags. Actual decoder restart remains exclusive; GPU-held references are retired before waiting for decoder exclusivity, and the mailbox is cleared before rebuilding the component.

Input frame-number gaps are logged. A gap resuming at IDR is forwarded with its key marker; an unexpected P-frame gap requests restart/IDR rather than knowingly feeding missing references. No queue size was increased and Direct Submit remains disabled for the blocking vendor path.

The client line-buffers stdout when piped to `tee`, emits a startup marker, and reports submit, queue and render timings. FFmpeg API timings include time spent inside the API: `avcodec_send_packet()` can invoke the vendor receive path internally, so its duration is **not** just the encoded-memory-copy duration.

Marker used by this earlier H.264 build (the current optimized build additionally includes `h264_filler=skip-valid`):

```text
TH1520 pipeline: render_control=independent h264_tail=max-two-zero packet_metadata=key+pts
```

## Verification

The local build is a native riscv64 Release executable. Tests included:

- CPU sender checks for every tail length from 0 to 1024, internal NAL/EPB preservation, invalid input, KEY/PTS retention across EAGAIN, idle polling and legacy/HEVC/non-TH1520 behavior.
- A deterministic CPU lock test: another thread holds the decoder mutex while presentation and stop must complete independently, repeated 20 times.
- Hardware DRM rendering with tail lengths 0, 1, 2, 3, 512 and 1024. All passed varied-pixel checks without `BYTE_STREAM` diagnostics.
- CABAC High-profile synthetic video: after per-AU padding and the production normalizer, software-decoded frame MD5 output exactly matched the original clip.
- Threaded, padded, eight-AU burst/fullscreen rendering, and padded CABAC/NV12 rendering.
- Twenty same-process padded CABAC/fullscreen startup-stop-cleanup runs with the new lock/control implementation; every run returned fd count to 4 and cleared all event registrations, with no `BYTE_STREAM` diagnostic.
- TH1520-enabled, TH1520-disabled X11/SDL, and GCC SDL-only builds, plus strict GCC syntax checks for the changed video sources.

The smoke test has additional environment options:

```sh
MOONLIGHT_TH1520_TEST_THREADED=1
MOONLIGHT_TH1520_TEST_TAIL_PADDING=512   # 0..1024 bytes, included in DU.fullLength
MOONLIGHT_TH1520_TEST_BURST=8           # 1..32 AUs at a time, same average target rate
MOONLIGHT_TH1520_TEST_FULLSCREEN=1
```

The CPU test/fixture commands are:

```sh
./build/th1520-lock-unit
./build/th1520-unit build/test-1080p.h264 build/test-stride.h264 build/test-720p.hevc
./build/th1520-normalize-fixture build/test-cabac.h264 build/fix-cabac-normalized.h264 512
ffmpeg -c:v h264 -i build/test-cabac.h264 -f framemd5 build/original.md5
ffmpeg -c:v h264 -i build/fix-cabac-normalized.h264 -f framemd5 build/normalized.md5
cmp build/original.md5 build/normalized.md5
```

These examples assume matching generated fixtures; video files and build outputs are intentionally not committed. The [main guide](<TH1520.md>) describes fixture generation. Burst smoke tests exercise the production callback/mailbox, but do not emulate the entire network/core decode-unit queue.

## Limited real-host results

The supplied, already-paired host was reachable and exposed `Desktop`. Tests used the private memory-fixed FFmpeg, view-only mode, `-nosops`, and bounded 8–20 second sessions. No keyboard/mouse input was sent and no desktop screenshots or compressed desktop samples were saved. Captured artifacts are diagnostic logs only.

| Test | Observed result |
| --- | --- |
| User's previous log | `decoded=406 presented=67 replaced=338`, repeated `BYTE_STREAM` and core queue overflow. |
| Updated 1080p60 H.264, 20 Mbps, 20-second process limit | `decoded=745 presented=729 replaced=15`, no `BYTE_STREAM`; **core queue overflow still occurred**. |
| Updated profiling run at the same requested format | Input inspection averaged 0.139 ms; send API averaged 24.081 ms; receive API averaged 0.006 ms. Rendering averaged 4.634 ms. |
| DRM versus NV12 | Both still exhibited input/submission backlog; NV12 was not a cure for the remaining issue. |
| Four-slice H.264 negotiation | Verified four slices in input NAL summaries, but no useful throughput gain; the experiment was reverted. |
| CSI/VSI low-delay extension tests in the private library | The tested configuration indices returned `0x8000101b` (unsupported index). All experimental changes were reverted; the installed private source diff was compared byte-for-byte with the original documented lifetime patch before rebuilding it. |
| Updated 1080p30 H.264, 10 Mbps, 10-second process limit | `decoded=266 presented=265 replaced=0`, no core queue overflow, input gaps 0; max observed queue age 57.427 ms. This is a short observation, not long-session certification. |
| Updated 720p60 H.264, 10 Mbps | Improved load but still two queue-overflow events during the short test; **not** labeled stable. |

These are not controlled visual/FPS comparisons against the user's earlier run: the desktop content, run duration and view-only setting can differ. The lock unit test independently verifies the synchronization change. New timings show that the remaining delay is in the vendor send/API call chain, not in the added size/tail inspection, and the current client still cannot guarantee the 16.7 ms budget required by the tested 1080p60 live stream. They do not establish whether the final bottleneck is inside SDK software, hardware/driver scheduling, or another operation invoked by the wrapper.

The HEVC real-stream comparison also retained SDK tail diagnostics; this follow-up deliberately normalizes H.264 only. Do not recommend codec auto-selection/HEVC as a proven workaround for this issue.

Audio setup/packet reception completed, but playback quality and A/V synchronization were not subjectively verified. Interactive input was intentionally disabled. Long live sessions, complex gameplay, 4K, and network-failure recovery remain unvalidated.

## Historical lower-load retest

The commands below record the conservative setting used before filler optimization. For the current tested 1080p60 setting and remaining validation limits, use the [main guide](<TH1520.md>) and [profiling follow-up](<TH1520-VENDOR-PROFILE.md>). Keep the private library prefix scoped to the process/shell.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TH1520_TESTS=ON
cmake --build build -j4
export DISPLAY=:0
export LD_LIBRARY_PATH="$PWD/build/ffmpeg-fixed/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./build/moonlight stream -platform th1520 -codec h264 -1080 -fps 30 -bitrate 10000 \
  -app Desktop -mapping third_party/SDL_GameControllerDB/gamecontrollerdb.txt \
  HOST -verbose 2>&1 | tee run-fixed.log
```

Use `-viewonly` for an initial video-only observation if interactive input is not needed. The new marker identifies the rebuilt path; `TH1520 timing` and `TH1520 API timing` at exit distinguish presentation, queue age, inspection and API-call delay. `MOONLIGHT_TH1520_TRACE=1` optionally prints only NAL types/lengths for the first five H.264 inputs, never picture bytes.

Local evidence includes [input unit results](<../build/fix-input-unit.log>), [lock unit results](<../build/fix-lock-unit.log>), [padded burst results](<../build/fix-burst.log>), [1080p60 real-host log](<../build/fix-real-stream.log>), [API profiling log](<../build/fix-live-profile.log>), and [1080p30 real-host log](<../build/fix-live-1080p30.log>). The generated artifacts are local evidence, not portable source deliverables.
