#!/usr/bin/env bash
#
# Build static archives of zlib, libjpeg-turbo and SDL2 from source, into a prefix that
# CMake links statically into the ta-engine binaries (see CMakeLists.txt,
# always, unconditionally). This lets the shipped binaries carry these libs internally so a
# host needs no matching libz / libjpeg / libSDL2 shared library installed -- handy for the
# release zips and for pinning known-good versions.
#
# NOTE on SDL2: SDL2 dlopens its VIDEO/AUDIO backends (X11, Wayland, ALSA, PulseAudio,
# PipeWire, ...) at runtime, so a statically-linked SDL2 still needs THOSE system libraries
# present to open a window / play audio -- static linking removes the libSDL2-2.0.so.0
# version dependency, not the windowing/audio stack. To BUILD SDL2 here, the corresponding
# -devel headers must be installed (Fedora: libX11-devel wayland-devel alsa-lib-devel
# pulseaudio-libs-devel pipewire-devel libXext-devel libXcursor-devel mesa-libGL-devel ...);
# whichever backends are missing at build time are simply left out.
#
# Env overrides:
#   ZLIB_VERSION      git tag         (default: v1.3.1)
#   JPEG_VERSION      git tag         (default: 3.0.4)          (libjpeg-turbo)
#   SDL2_VERSION      git tag         (default: release-2.30.9)
#   PREFIX            install prefix  (default: <repo>/third_party/static-deps)
#   SRC               source dir      (default: <PREFIX>/src)
#   JOBS              parallel jobs   (default: nproc)
#
# Idempotent per-library: each build is skipped if its installed marker already exists, so
# CI can cache $PREFIX and only rebuild what changed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # repo root
ZLIB_VERSION="${ZLIB_VERSION:-v1.3.1}"
JPEG_VERSION="${JPEG_VERSION:-3.0.4}"
SDL2_VERSION="${SDL2_VERSION:-release-2.32.10}"   # 2.32.x tracks the current PipeWire API (GCC-strict)
PREFIX="${PREFIX:-$here/third_party/static-deps}"
SRC="${SRC:-$PREFIX/src}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

mkdir -p "$SRC"

# --- zlib (static libz.a + zlib.pc) -------------------------------------------
if [ -f "$PREFIX/lib/libz.a" ]; then
  echo "static-deps: zlib already built"
else
  echo "static-deps: building zlib $ZLIB_VERSION -> $PREFIX"
  z="$SRC/zlib"
  [ -d "$z/.git" ] || git clone --depth 1 --branch "$ZLIB_VERSION" https://github.com/madler/zlib.git "$z"
  ( cd "$z"
    # zlib's own configure; --static builds ONLY the archive (no libz.so).
    CFLAGS="-O2 -fPIC" ./configure --static --prefix="$PREFIX"
    make -j"$JOBS"
    make install )
fi

# --- libjpeg-turbo (static libjpeg.a + libjpeg.pc, standard libjpeg API) ------
if [ -f "$PREFIX/lib/libjpeg.a" ] || [ -f "$PREFIX/lib64/libjpeg.a" ]; then
  echo "static-deps: libjpeg-turbo already built"
else
  echo "static-deps: building libjpeg-turbo $JPEG_VERSION -> $PREFIX"
  j="$SRC/libjpeg-turbo"
  [ -d "$j/.git" ] || git clone --depth 1 --branch "$JPEG_VERSION" https://github.com/libjpeg-turbo/libjpeg-turbo.git "$j"
  cmake -S "$j" -B "$j/build" -G "${CMAKE_GENERATOR:-Unix Makefiles}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  cmake --build "$j/build" -j"$JOBS"
  cmake --install "$j/build"
fi

# --- SDL2 (static libSDL2.a + SDL2Config.cmake + sdl2.pc) ---------------------
# SKIP_SDL2=1 builds only the leaf libs (zlib/libjpeg) -- useful before the SDL2 audio-
# backend -devel headers are installed, so an audio-less SDL2 isn't cached.
if [ -n "${SKIP_SDL2:-}" ]; then
  echo "static-deps: SKIP_SDL2 set -- skipping SDL2"
elif [ -f "$PREFIX/lib/libSDL2.a" ] || [ -f "$PREFIX/lib64/libSDL2.a" ]; then
  echo "static-deps: SDL2 already built"
else
  echo "static-deps: building SDL2 $SDL2_VERSION -> $PREFIX"
  s="$SRC/SDL"
  [ -d "$s/.git" ] || git clone --depth 1 --branch "$SDL2_VERSION" https://github.com/libsdl-org/SDL.git "$s"
  # SDL_STATIC_PIC so the .a links into our (PIE) binaries; keep the dynamic-API/dlopen
  # backend loading (the default) so present X11/Wayland/audio libs are used at runtime.
  cmake -S "$s" -B "$s/build" -G "${CMAKE_GENERATOR:-Unix Makefiles}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_STATIC_PIC=ON \
    -DSDL_TEST=OFF
  cmake --build "$s/build" -j"$JOBS"
  cmake --install "$s/build"
fi

echo "static-deps: done -> $PREFIX (zlib + libjpeg-turbo + SDL2 static archives)"
echo "  the engine links these automatically: cmake -B build -G Ninja"
