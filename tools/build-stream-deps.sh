#!/usr/bin/env bash
# Static streaming dependencies. GPU vendor runtimes remain OS/driver supplied.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$here/third_party/ffmpeg-bink}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
root="$PREFIX/stream-src"
mkdir -p "$root" "$PREFIX/include"
fetch() {
  if [ ! -d "$root/$1/.git" ]; then
    git clone --depth 1 --branch "$3" "$2" "$root/$1"
  fi
}
# Fixed source revision; no GPL-incompatible codecs. TAK is GPLv3.
if [ ! -f "$PREFIX/lib/libx264.a" ]; then
  mkdir -p "$root/x264"
  (cd "$root/x264"
   git init
   git fetch --depth 1 https://github.com/mirror/x264.git c24e06c2e184345ceb33eb20a15d1024d9fd3497
   git checkout --detach FETCH_HEAD
   args=()
   [ -n "${CROSS_PREFIX:-}" ] && args+=("--cross-prefix=$CROSS_PREFIX" "--host=${CROSS_PREFIX%-}")
   # MSYS2's shell runs as x64 even on Windows ARM64. config.guess would
   # therefore select x86 code/flags for the native CLANGARM64 compiler.
   if [ -z "${CROSS_PREFIX:-}" ] && [ "${TARGET_OS:-}" = mingw32 ] && [ -n "${TARGET_ARCH:-}" ]; then
     args+=("--host=$TARGET_ARCH-w64-mingw32")
   fi
   ./configure --prefix="$PREFIX" --enable-static --enable-pic --disable-cli --disable-opencl --disable-asm ${args[@]+"${args[@]}"}
   make -j"$JOBS"
   make install-lib-static)
fi
platform="${TARGET_OS:-$(uname -s)}"
# Static OpenSSL on every platform: multiplayer TLS everywhere, and FFmpeg's
# RTMPS on Linux and macOS (Windows FFmpeg uses Schannel).
openssl_version=3.5.8
if [ ! -f "$PREFIX/lib/libssl.a" ] || [ "$(cat "$PREFIX/openssl-version" 2>/dev/null || true)" != "$openssl_version" ]; then
  fetch "openssl-$openssl_version" https://github.com/openssl/openssl.git "openssl-$openssl_version"
  (cd "$root/openssl-$openssl_version"
   tls_args=(--prefix="$PREFIX" --libdir=lib no-shared no-module no-tests no-apps)
   # A cross build (CROSS_PREFIX, as for x264 above) must name the target compiler,
   # or OpenSSL's Configure uses the host cc for the mingw64 target.
   [ -n "${CROSS_PREFIX:-}" ] && tls_args+=("--cross-compile-prefix=$CROSS_PREFIX")
   case "$platform" in
     MINGW*|MSYS*|mingw32)
       if [ "${TARGET_ARCH:-}" = aarch64 ]; then
         cat > Configurations/99-tak.conf <<'TLSCONFIG'
my %targets = (
    "tak-mingw-arm64" => {
        inherit_from => [ "mingw-common" ],
        sys_id => "MINGW64", bn_ops => "SIXTY_FOUR_BIT",
        asm_arch => "aarch64", uplink_arch => undef,
    },
);
TLSCONFIG
         perl Configure tak-mingw-arm64 no-asm "${tls_args[@]}"
       else
         perl Configure mingw64 "${tls_args[@]}"
       fi ;;
     Darwin|darwin)
       CFLAGS="${CFLAGS:-} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET:-14.0}" ./config "${tls_args[@]}" ;;
     *) ./config "${tls_args[@]}" ;;
   esac
   make -j"$JOBS"
   make install_sw)
  printf '%s\n' "$openssl_version" > "$PREFIX/openssl-version"
fi
case "$platform" in
  Darwin|darwin) exit 0 ;;
esac
fetch nvcodec https://github.com/FFmpeg/nv-codec-headers.git n12.1.14.1
make -C "$root/nvcodec" PREFIX="$PREFIX" install
case "$platform" in
  MINGW*|MSYS*|mingw32)
    fetch amf https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git v1.4.35
    mkdir -p "$PREFIX/include/AMF"
    cp -R "$root/amf/amf/public/include/." "$PREFIX/include/AMF/"
    fetch vpl https://github.com/intel/libvpl.git v2.14.0
    # An undefined _MSC_VER is zero in MinGW. Upstream's old-MSVC fallback
    # then shadows real CRT functions and breaks newer Windows SDK headers.
    sed -i.bak 's/^#if _MSC_VER < 1400$/#if defined(_MSC_VER) \&\& _MSC_VER < 1400/' \
      "$root/vpl/libvpl/src/windows/mfx_dispatcher_defs.h"
    vpl_args=()
    if [ -n "${CROSS_PREFIX:-}" ]; then
      vpl_args+=(-DCMAKE_SYSTEM_NAME=Windows "-DCMAKE_C_COMPILER=${CROSS_PREFIX}gcc" "-DCMAKE_CXX_COMPILER=${CROSS_PREFIX}g++")
    fi
    cmake -S "$root/vpl" -B "$root/vpl/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
      -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF ${vpl_args[@]+"${vpl_args[@]}"}
    cmake --build "$root/vpl/build" -j"$JOBS"
    cmake --install "$root/vpl/build"
    ;;
  *)
    # VA-API: FFmpeg compiles against pinned libva HEADERS, and the prefix's libva.a
    # is tools/vashim.c, which forwards to the SYSTEM libva.so.2 at run time. A
    # bundled libva loader could not initialise drivers built for a newer libva and
    # knew only fixed driver folders. No libva/libdrm code is linked into the game.
    va_version=2.22.0
    fetch "va-$va_version" https://github.com/intel/libva.git "$va_version"
    shim_stamp="$va_version-$(cksum < "$here/tools/vashim.c")"
    if [ "$(cat "$PREFIX/vashim-version" 2>/dev/null || true)" != "$shim_stamp" ]; then
      # Drop the real libva/libdrm a cached prefix from an older recipe still holds.
      rm -rf "$PREFIX/include/va" "$PREFIX/include/libdrm" "$PREFIX/include/xf86drm.h" \
        "$PREFIX/include/xf86drmMode.h" "$PREFIX/include/libsync.h" "$PREFIX/lib/dri"
      rm -f "$PREFIX"/lib/libva*.so* "$PREFIX"/lib/libva*.a "$PREFIX"/lib/libdrm*.so* "$PREFIX"/lib/libdrm*.a \
        "$PREFIX"/lib/pkgconfig/libva*.pc "$PREFIX"/lib/pkgconfig/libdrm*.pc
      mkdir -p "$PREFIX/include/va" "$PREFIX/lib/pkgconfig"
      vasrc="$root/va-$va_version"
      for h in va.h va_backend.h va_backend_prot.h va_backend_vpp.h va_compat.h \
               va_dec_hevc.h va_dec_jpeg.h va_dec_vp8.h va_dec_vp9.h va_dec_av1.h va_dec_vvc.h \
               va_drmcommon.h va_egl.h va_enc_hevc.h va_enc_h264.h va_enc_jpeg.h va_enc_vp8.h \
               va_fei.h va_fei_h264.h va_fei_hevc.h va_enc_mpeg2.h va_enc_vp9.h va_enc_av1.h \
               va_str.h va_tpi.h va_prot.h va_vpp.h drm/va_drm.h; do
        cp "$vasrc/va/$h" "$PREFIX/include/va/"
      done
      api() { sed -n "s/^va_api_$1_version = \([0-9]*\)$/\1/p" "$vasrc/meson.build"; }
      api_version="$(api major).$(api minor).$(api micro)"
      sed -e "s/@VA_API_MAJOR_VERSION@/$(api major)/" -e "s/@VA_API_MINOR_VERSION@/$(api minor)/" \
          -e "s/@VA_API_MICRO_VERSION@/$(api micro)/" -e "s/@VA_API_VERSION@/$api_version/g" \
          "$vasrc/va/va_version.h.in" > "$PREFIX/include/va/va_version.h"
      "${CC:-cc}" -std=gnu11 -O2 -fPIC -fvisibility=hidden -Wall -I"$PREFIX/include" \
        -c "$here/tools/vashim.c" -o "$root/vashim.o"
      "${AR:-ar}" rcs "$PREFIX/lib/libva.a" "$root/vashim.o"
      # libva-drm's one function lives in the same archive.
      cat > "$PREFIX/lib/pkgconfig/libva.pc" <<VAPC
prefix=$PREFIX
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: va
Description: VA-API headers with a forwarder to the system libva.so.2 (tools/vashim.c)
Version: $api_version
Libs: -L\${libdir} -lva -ldl -pthread
Cflags: -I\${includedir}
VAPC
      cat > "$PREFIX/lib/pkgconfig/libva-drm.pc" <<VAPC
prefix=$PREFIX
includedir=\${prefix}/include

Name: va-drm
Description: VA-API DRM header; vaGetDisplayDRM forwards to the system libva-drm.so.2
Version: $api_version
Requires: libva
Cflags: -I\${includedir}
VAPC
      printf '%s\n' "$shim_stamp" > "$PREFIX/vashim-version"
    fi
    ;;
esac
