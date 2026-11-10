#!/bin/sh
# Build the matching RevyOS OMX lifetime fixes without replacing system libraries.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
REV=f86f86b3f7087b4e809a079371c4bc513d3d50f8
SOURCE="$ROOT/build/revyos-ffmpeg"
OBJECTS="$ROOT/build/ffmpeg-obj"
PREFIX="$ROOT/build/ffmpeg-fixed"
SDK="$ROOT/build/omx-sdk"
PATCH="$ROOT/docs/patches/revyos-ffmpeg-omx-lifetime.patch"

for tool in git gcc make pkg-config dpkg-deb apt-get; do
  command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
pkg-config --exists libdrm || { echo 'Install libdrm-dev first.' >&2; exit 1; }
mkdir -p "$ROOT/build" "$OBJECTS" "$SDK"
if [ ! -d "$SOURCE/.git" ]; then
  if [ -e "$SOURCE" ]; then
    echo "Refusing to overwrite existing non-Git source directory: $SOURCE" >&2
    exit 1
  fi
  git clone --depth 1 --branch revyos-ffmpeg5.1.4-v3 https://github.com/revyos/ffmpeg.git "$SOURCE"
fi
if [ "$(git -C "$SOURCE" rev-parse HEAD)" != "$REV" ]; then
  echo "Expected RevyOS FFmpeg commit $REV; use a separate build directory for another revision." >&2
  exit 1
fi
if git -C "$SOURCE" apply --reverse --check "$PATCH" 2>/dev/null; then
  echo 'OMX lifetime patch is already applied.'
else
  git -C "$SOURCE" apply --check "$PATCH"
  git -C "$SOURCE" apply "$PATCH"
fi

# RevyOS ships vendor extensions in th1520-vpu, but base OMX headers are in
# libomxil-bellagio-dev. Download/unpack headers if that package isn't installed.
if [ -f /usr/include/OMX_Core.h ]; then
  OMX_INCLUDE=/usr/include
else
  if [ ! -f "$SDK/usr/include/OMX_Core.h" ]; then
    mkdir -p "$SDK/packages"
    (
      cd "$SDK/packages"
      apt-get download libomxil-bellagio-dev
      set -- ./libomxil-bellagio-dev_*.deb
      [ "$#" -eq 1 ] && [ -f "$1" ] || { echo 'Expected exactly one Bellagio development package.' >&2; exit 1; }
      dpkg-deb --extract "$1" "$SDK"
    )
  fi
  OMX_INCLUDE="$SDK/usr/include"
fi
[ -f /usr/include/omxil/OMX_CsiExt.h ] || { echo 'Install th1520-vpu first.' >&2; exit 1; }

# OMX's shared loader counter is defined in omx.o, which this vendor tree builds
# with the encoder. Keep that small encoder enabled alongside both decoders.
(
  cd "$OBJECTS"
  "$SOURCE/configure" --prefix="$PREFIX" \
    --disable-programs --disable-doc --disable-network --disable-avdevice \
    --disable-avfilter --disable-avformat --disable-swresample --disable-swscale \
    --disable-postproc --disable-everything --disable-autodetect \
    --enable-shared --disable-static --disable-debug --disable-asm --cc=gcc \
    --enable-omx --enable-libdrm \
    --enable-decoder=h264,hevc,h264_omx,hevc_omx --enable-encoder=h264_omx \
    --enable-parser=h264,hevc --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
    --extra-cflags="-DDRM_PRIME -I$OMX_INCLUDE"
  make -j"${JOBS:-4}"
  make install
)
printf '\nInstalled private FFmpeg libraries in %s\n' "$PREFIX"
printf 'Use only for Moonlight, not as a system-wide replacement:\n'
printf '  LD_LIBRARY_PATH="%s/lib" ./build/moonlight stream -platform th1520 <host>\n' "$PREFIX"
