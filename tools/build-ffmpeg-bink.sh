#!/usr/bin/env bash
# Static FFmpeg for retail Bink playback, H.264/HEVC/AAC RTMPS streaming and the
# streaming panel's local MP4 copy (fragmented, so a crash still leaves a playable file).
# Builds pinned static dependencies using build-stream-deps.sh. Only OS frameworks
# and GPU driver runtimes remain external. PREFIX / JOBS / cross knobs supported.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # repo root
FFMPEG_VERSION="${FFMPEG_VERSION:-n7.1.1}"
PREFIX="${PREFIX:-$here/third_party/ffmpeg-bink}"
SRC="${SRC:-$PREFIX/src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

recipe=$(cat "$here/tools/build-ffmpeg-bink.sh" "$here/tools/build-stream-deps.sh" "$here/tools/ffmpeg-tls-hostname.patch" \
  "$here/tools/vashim.c" | cksum)
stamp="stream-v2-$FFMPEG_VERSION-${TARGET_OS:-$(uname -s)}-${TARGET_ARCH:-native}-$recipe"
if [ -f "$PREFIX/stream-build-version" ] && [ "$(cat "$PREFIX/stream-build-version")" = "$stamp" ]; then
  echo "static FFmpeg with streaming: already built"; exit 0
fi
PREFIX="$PREFIX" JOBS="$JOBS" "$here/tools/build-stream-deps.sh"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

echo "ffmpeg-bink: building $FFMPEG_VERSION -> $PREFIX"
mkdir -p "$SRC"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch "$FFMPEG_VERSION" https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi

# Cross-compile pass-through (empty on a normal native build).
cross_args=()
[ -n "${CROSS_PREFIX:-}" ] && cross_args+=(--enable-cross-compile "--cross-prefix=$CROSS_PREFIX" "--pkg-config=${PKG_CONFIG:-pkg-config}")
[ -n "${TARGET_OS:-}" ]    && cross_args+=("--target-os=$TARGET_OS")
[ -n "${TARGET_ARCH:-}" ]  && cross_args+=("--arch=$TARGET_ARCH")
[ -n "${CC:-}" ]           && cross_args+=("--cc=$CC")

platform="${TARGET_OS:-$(uname -s)}"
stream_args=()
case "$platform" in
  # macOS TLS is the static OpenSSL (Apple deprecated SecureTransport, whose FFmpeg 7.1
  # backend also blocked RTMP's non-blocking reads). OpenSSL never reads the keychain:
  # src/video/stream.cpp hands it the system trust anchors as its ca_file.
  Darwin|darwin) stream_args+=(--enable-openssl --enable-videotoolbox --enable-encoder=h264_videotoolbox,hevc_videotoolbox) ;;
  MINGW*|MSYS*|mingw32)
    stream_args+=(--enable-schannel --enable-d3d11va --enable-dxva2 --enable-amf --enable-libvpl --enable-encoder=h264_amf,h264_qsv,hevc_amf,hevc_qsv)
    # FFmpeg 7.1 supports Windows NVENC only on x86; its configure script
    # explicitly disables ffnvcodec on Windows ARM64.
    if [ "${TARGET_ARCH:-}" != aarch64 ]; then
      stream_args+=(--enable-nvenc --enable-ffnvcodec --enable-encoder=h264_nvenc,hevc_nvenc)
    fi
    ;;
  *) stream_args+=(--enable-openssl --enable-nvenc --enable-ffnvcodec --enable-vaapi --enable-encoder=h264_nvenc,h264_vaapi,hevc_nvenc,hevc_vaapi) ;;
esac
cd "$SRC"
# FFmpeg 7.1 verifies the certificate chain but not the hostname with OpenSSL.
# The patch also loads the OS trust store. Never disable certificate validation.
if ! grep -q 'SSL_set1_host' libavformat/tls_openssl.c; then
  patch -p1 < "$here/tools/ffmpeg-tls-hostname.patch"
fi
./configure \
  --prefix="$PREFIX" \
  --disable-shared --enable-static --enable-pic --enable-small \
  --disable-programs --disable-doc --disable-htmlpages --disable-manpages --disable-txtpages \
  --disable-everything --enable-network --disable-autodetect --disable-debug \
  --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 \
  --disable-audiotoolbox --disable-avfoundation --disable-coreimage --disable-appkit \
  --enable-gpl --enable-version3 --enable-libx264 --pkg-config-flags=--static \
  --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
  --enable-swscale --enable-swresample \
  --enable-encoder=libx264,aac --enable-muxer=flv,mp4 \
  --enable-protocol=file,rtmp,rtmps,tcp,tls \
  "${stream_args[@]}" \
  --enable-demuxer=smacker \
  --enable-decoder=smackvideo,smackaudio \
  ${cross_args[@]+"${cross_args[@]}"}   # 3.2-safe empty-array expansion (macOS ships Bash 3.2)

make -j"$JOBS"
make install
printf '%s\n' "$stamp" > "$PREFIX/stream-build-version"
echo "ffmpeg-bink: done -> $PREFIX/lib (static .a + pkgconfig)"
