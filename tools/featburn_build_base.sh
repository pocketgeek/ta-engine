#!/usr/bin/env bash
# featburn_build_base.sh BASE_SRC BASE_BUILD OUT_DIR -- build tools/featburn_bench.cpp (and
# tools/featgen_test.cpp) from THIS tree against a base tree that predates them: the base's
# headers and its already-built libraries (tak-formats, tak-crypto, tak_tls), with the o2
# flags of the CMake build. Gives a base featburn_bench for tools/featburn_timing.sh.
# The static-deps/ffmpeg-bink prefixes come from BASE_BUILD's CMakeCache.txt (default: BASE_SRC/third_party).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$1 build=$2 out=$3
mkdir -p "$out"
cache() { sed -n "s/^$1:[A-Z]*=//p" "$build/CMakeCache.txt"; }
sd=$(cache TAK_STATIC_DEPS_PREFIX); fb=$(cache TAK_FFMPEG_PREFIX)
sd=${sd:-$src/third_party/static-deps}; fb=${fb:-$src/third_party/ffmpeg-bink}
for t in featburn_bench featgen_test; do
  c++ -DTAK_HAVE_BUILD_ID_H=1 '-DTAK_VERSION="0.9.12"' -I"$src/src" -I"$build/generated" -isystem "$sd/include" \
      -O2 -g -std=gnu++20 -Wall -Wextra -ffp-contract=off -c "$here/tools/$t.cpp" -o "$out/$t.o"
  c++ -O2 -g -fuse-ld=mold -static-libgcc -static-libstdc++ "$out/$t.o" -o "$out/$t" \
      "$build/libtak-formats.a" "$sd/lib/libz.a" "$sd/lib64/libjpeg.a" "$build/libtak-crypto.a" \
      "$build/libtak_tls.a" "$fb/lib/libssl.a" "$fb/lib/libcrypto.a" -ldl
done
echo "built $out/featburn_bench $out/featgen_test against $src"
