# Moonlight Embedded

 [![Build](https://img.shields.io/github/actions/workflow/status/moonlight-stream/moonlight-embedded/build.yml?branch=master)](https://github.com/moonlight-stream/moonlight-embedded/actions/workflows/build.yml?query=branch%3Amaster) [Nightly Build Downloads](https://nightly.link/moonlight-stream/moonlight-embedded/workflows/build/master)

Moonlight Embedded is an open source client for [Sunshine](https://github.com/LizardByte/Sunshine) and NVIDIA GameStream for embedded Linux systems, like Raspberry Pi, CuBox-i and ODROID. Moonlight allows you to stream your full collection of games and applications from your PC to other devices to play them remotely.

Moonlight also has [PC](https://github.com/moonlight-stream/moonlight-qt), [Android](https://github.com/moonlight-stream/moonlight-android), and [iOS](https://github.com/moonlight-stream/moonlight-ios) clients.

## Documentation

More information about installing and running Moonlight Embedded is available on the [wiki](https://github.com/moonlight-stream/moonlight-embedded/wiki).

## TH1520 / RevyOS

The `th1520` platform uses the RevyOS FFmpeg `h264_omx` and `hevc_omx` decoders, Bellagio OpenMAX IL and the `th1520-vpu` package. Video is displayed through X11 and EGL. Stock RevyOS FFmpeg defaults to NV12 hardware decode; the optional local FFmpeg lifetime fix enables automatic dma-buf/DRM PRIME output without the stock wrapper's per-frame memory growth. No V4L2 decoder or GStreamer pipeline is required.

Install the build dependencies on RevyOS:

```sh
sudo apt install cmake pkg-config build-essential \
  libavcodec-dev libavutil-dev libx11-dev libegl-dev libgles-dev libdrm-dev \
  libevdev-dev libudev-dev libopus-dev libasound2-dev libpulse-dev libsdl2-dev \
  libcurl4-gnutls-dev libssl-dev libexpat1-dev libavahi-client-dev uuid-dev
```

The `th1520-vpu` package, its kernel modules and an X11 desktop with EGL dma-buf import support must already be installed. Build with:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

From an X11 terminal, stream with:

```sh
./build/moonlight stream -platform th1520 -codec h264 -1080 -fps 30 -bitrate 10000 \
  -mapping third_party/SDL_GameControllerDB/gamecontrollerdb.txt <host>
```

When starting over SSH, export the existing desktop display first (usually `DISPLAY=:0`). The backend supports 8-bit H.264 and HEVC SDR. AV1, 10-bit video and HDR are rejected explicitly. Set `ENABLE_TH1520=OFF` to build without this backend.

See the [TH1520 guide](<docs/TH1520.md>) for prerequisites and the [real-stream follow-up](<docs/TH1520-STREAM-FIX.md>) for H.264 tail-padding/control-lock fixes and their measured limits. The tested 1080p60 live stream still overflows its input queue; local synthetic 60 FPS tests are not a guarantee of live-stream throughput.

## Bugs

Please check the wiki and old bug reports before submitting a new bug report.

Bugs can be reported to the [issue tracker](https://github.com/moonlight-stream/moonlight-embedded/issues).

## See also

[Moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) is the shared codebase between different Moonlight implementations

## Contribute

1. Fork us
2. Write code
3. Send Pull Requests
