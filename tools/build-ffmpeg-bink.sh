#!/usr/bin/env bash
#
# Build a MINIMAL, static, LGPL FFmpeg that can decode the retail door videos
# (Bink1 / "BIKf") and nothing else, then install it to a prefix that CMake links
# statically into taclient (always -- see CMakeLists.txt). This lets a
# shipped taclient play the door videos with NO runtime FFmpeg dependency, so a
# stock distro (whose libavcodec-free lacks the Bink decoder) needs nothing extra.
#
# Only the Bink demuxer + bink/binkaudio decoders + swscale/swresample are enabled, so
# no GPL codecs and no external media libraries are pulled in -- the result is pure
# LGPL, which is fine to static-link into an open-source, rebuildable binary.
#
# Env overrides:
#   FFMPEG_VERSION   git tag to build            (default: n7.1.1)
#   PREFIX           install prefix              (default: <repo>/third_party/ffmpeg-bink)
#   SRC              source/checkout dir         (default: <PREFIX>/src)
#   JOBS             parallel make jobs          (default: nproc)
#   CC / CROSS_PREFIX / TARGET_OS / TARGET_ARCH  cross-compile knobs (CI: Windows/macOS)
#
# Idempotent: if $PREFIX/lib/pkgconfig/libavcodec.pc already exists it does nothing, so
# CI can cache $PREFIX and skip the ~minutes-long rebuild.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # repo root
FFMPEG_VERSION="${FFMPEG_VERSION:-n7.1.1}"
PREFIX="${PREFIX:-$here/third_party/ffmpeg-bink}"
SRC="${SRC:-$PREFIX/src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ -f "$PREFIX/lib/pkgconfig/libavcodec.pc" ]; then
  echo "ffmpeg-bink: already built at $PREFIX (delete it to rebuild)"; exit 0
fi

echo "ffmpeg-bink: building $FFMPEG_VERSION -> $PREFIX"
mkdir -p "$SRC"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch "$FFMPEG_VERSION" https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi

# Cross-compile pass-through (empty on a normal native build).
cross_args=()
[ -n "${CROSS_PREFIX:-}" ] && cross_args+=(--enable-cross-compile "--cross-prefix=$CROSS_PREFIX")
[ -n "${TARGET_OS:-}" ]    && cross_args+=("--target-os=$TARGET_OS")
[ -n "${TARGET_ARCH:-}" ]  && cross_args+=("--arch=$TARGET_ARCH")
[ -n "${CC:-}" ]           && cross_args+=("--cc=$CC")

cd "$SRC"
./configure \
  --prefix="$PREFIX" \
  --disable-shared --enable-static --enable-pic --enable-small \
  --disable-programs --disable-doc --disable-htmlpages --disable-manpages --disable-txtpages \
  --disable-everything --disable-network --disable-autodetect --disable-asm --disable-debug \
  --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 \
  --disable-videotoolbox --disable-audiotoolbox --disable-avfoundation \
  --disable-coreimage --disable-appkit --disable-securetransport \
  --enable-swscale --enable-swresample \
  --enable-demuxer=bink \
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft \
  ${cross_args[@]+"${cross_args[@]}"}   # 3.2-safe empty-array expansion (macOS ships Bash 3.2)

make -j"$JOBS"
make install
echo "ffmpeg-bink: done -> $PREFIX/lib (static .a + pkgconfig)"
