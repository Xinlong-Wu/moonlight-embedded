# TH1520 on-device validation

## Scope

The historical results below were collected on the current LicheePi 4A on 2026-11-10, before a target host was available. They establish native compilation, hardware decoding and real X11/EGL rendering using the production callback path, **not an end-to-end streaming result**. A [2026-11-11 real-stream follow-up](<TH1520-STREAM-FIX.md>) has since added input/control-lock fixes and limited view-only host tests; it also records remaining 1080p60 queue overflow. Do not interpret the local synthetic results below as proof of sustained live-stream throughput.

Environment:

- riscv64, four T-Head C910 cores; kernel `6.6.140-th1520`.
- `th1520-vpu 2.0.2`, Bellagio `0.9.3-8revyos2`, accessible Hantro/VC8000D/vidmem devices.
- Stock RevyOS FFmpeg `7:5.1.4-0+deb12u1revyos3`.
- Xfce/X11, PowerVR BXM-4-64, EGL dma-buf import and external GLES textures.
- Isolated repaired FFmpeg built from RevyOS commit `f86f86b3f7087b4e809a079371c4bc513d3d50f8`, with the [lifetime patch](<patches/revyos-ffmpeg-omx-lifetime.patch>). System libraries were not replaced.

## Results

| Scenario | Result | Evidence/interpretation |
| --- | --- | --- |
| Native TH1520-enabled Release build | Pass | Clang 22; native riscv64 executable. |
| `ENABLE_TH1520=OFF`, X11/SDL enabled | Pass | Builds without the TH1520 DRM code/dependency. |
| GCC 14, TH1520 and X11 disabled | Pass | SDL-only regression configuration. An existing evdev `strncpy` warning remains. |
| H.264 720p60 DRM PRIME and NV12 | Pass | Actual Hantro component loaded; mapped-window pixel checks and screenshots show a varied color test pattern. |
| H.264 1080p60 DRM PRIME and NV12 | Pass | Production decode/render callbacks; no software decoder substitution. |
| H.264 1366×768 DRM PRIME and NV12 | Pass | Non-64-multiple visible width, padded decoder stride; no stripe/green-screen failure in the captured image. |
| HEVC Main 720p30 and 1080p30, DRM PRIME/NV12 | Pass | Production callbacks, including independent producer/main-renderer threads. |
| Twenty same-process restarts | Pass | H.264/NV12, HEVC/DRM and HEVC/NV12 each completed 20 runs; event registrations cleared and fd count returned to 4. |
| Twenty threaded 1080p60 H.264/DRM restarts with repaired library | Pass | Worker submits, main thread renders. Every run returned fd count to 4, with zero mailbox replacements at normal pacing. |
| Delayed UI with a 100 ms test stall | Pass | 180 submitted AUs, 178 decoded, 44 presented, 134 replaced; no VPU starvation or fd leak. Replacements occur only after decode, not on compressed input. |
| CPU-only ownership and dimension tests | Pass | Mocked EAGAIN retention/retry, idle polling (not EOS), legacy retry safety, visible/coded size guard, invalid configuration rejection, NV12 normalization, and AV1/Main10/4:4:4 rejection. |
| Stock FFmpeg DRM PRIME, 1080p60, 600 s | **Memory growth reproduced** | 36,000 submitted AUs; RSS grew from 79,360 KiB at 10 s to 103,488 KiB at 600 s. fd count alone did not expose the leak. |
| Repaired FFmpeg DRM PRIME, 1080p60, 600 s | Pass within measured scope | 36,000 AUs submitted in 600,000 ms. RSS reached 68,444 KiB at 20 s and stayed at exactly that value through 600 s. fd count returned from 4 to 4 after cleanup. |
| Stock FFmpeg automatic/NV12 fallback, 1080p60, 120 s | Functional pass; shorter observation | The default selected hardware NV12, not software. The test completed and fd count returned to 4. This shorter run is not a claim that all stock wrapper allocation issues have been repaired. |
| Private build script rerun | Pass | Recognized the existing patch and rebuilt/reinstalled only inside the private build prefix. |

The upstream RevyOS FFmpeg source still emits its own compiler warnings; this work does not claim that the entire vendor tree is warning-free. The modified Moonlight video sources also passed strict GCC syntax checks with `-Werror`.

## What the smoke test proves

The [smoke test](<../tests/th1520.c>) uses the same `decoder_callbacks_x11_th1520`, linked-list Decode Units, frame mailbox, EGL image import, fence handling, stop and cleanup functions as the application. It does not shell out to `gst-launch` or substitute a separate renderer. The test checks the actual mapped X11 window with `XGetImage` and rejects black, flat or insufficiently varied patterns.

A session stop does not send file-playback EOS/drain into the live-stream decoder. Consequently, its reported decoded-frame count can trail the submitted count by a few frames at the end of a short run. This is not an assertion of packet loss or of complete file-playback drain semantics. Presentation counts are reported separately from input counts; a successful submission alone is not counted as a presented frame.

Threaded tests exercise a decoder worker and a main-thread renderer. The deliberate UI-delay test verifies that decoded frames can be replaced without filling a pipe with retained OMX buffers. It does not emulate all possible network/driver failure interleavings.

## Reproducible artifacts in this checkout

The build directory is intentionally Git-ignored. Relevant local artifacts include:

- [Native Moonlight executable](<../build/moonlight>).
- [Private FFmpeg library resolution](<../build/fixed-libraries.txt>).
- [Stock DRM memory-growth log](<../build/1080p-long.log>).
- [Repaired DRM ten-minute log](<../build/fixed-drm-long.log>).
- [Stock auto/NV12 log](<../build/stock-auto-long.log>).
- [Threaded twenty-run log](<../build/fixed-threaded.log>).
- [Delayed-renderer stress log](<../build/mailbox-stress.log>).
- [CPU regression results](<../build/unit-results.log>).
- [Repaired DRM window capture](<../build/fixed-drm-long.png>).

Use the [build/run guide](<TH1520.md>) and [private dependency build script](<../scripts/build-th1520-ffmpeg.sh>) to reproduce these after cleaning the build directory. The source and tests, not generated binaries or sample video files, are the portable deliverables.

## Not validated / intentionally unsupported

- Fresh pairing flow, long real-host streaming and controlled network-loss/IDR recovery. Basic access to an already-paired host and limited view-only streaming have since been exercised in the follow-up.
- Audio output and A/V synchronization during an actual stream.
- Remote keyboard, mouse, gamepad behavior and user-perceived end-to-end latency.
- 4K throughput, HDR, 10-bit, AV1, 4:4:4, Wayland or direct TTY/KMS output.
- Midstream stream-resolution changes: deliberately rejected before OMX because the vendor reconfiguration path can return GPU-held output buffers prematurely. Start a new stream for a new resolution.
- Actual GPU context loss, hardware bus errors or proprietary OMX deadlocks. The wrapper reports failures and avoids knowingly restarting over outstanding GPU references, but cannot time-limit a blocked vendor call from inside the shared library.
- Full colorimetric accuracy or resistance to every malformed/multi-SPS bitstream. The public parser is a conservative size guard, not a complete security validator.

Limited real-host tests are now recorded in the [follow-up](<TH1520-STREAM-FIX.md>). The presentation starvation and H.264 long-zero-tail diagnostics improved, but the tested 1080p60 stream still exceeds the observed vendor send-path budget. Initial retesting should use the documented lower-load H.264 mode rather than treating the entire end-to-end path as certified.
