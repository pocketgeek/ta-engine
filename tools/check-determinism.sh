#!/bin/sh
# Cross-build determinism check for the simulation's deterministic math shim.
# See docs/detmath-scope.md. Two parts:
#
#   1. Guard  -- the sim path must not call libm transcendentals directly
#      (they are not correctly-rounded and would break cross-build lockstep).
#   2. Golden -- build the detmath shim standalone with every available
#      compiler x optimization level (and aarch64 under qemu when present) and
#      assert every build emits the SAME golden hash.
#
# A leg is SKIPPED (not failed) when its toolchain is absent, so this runs
# anywhere. CI installs clang for the gcc-vs-clang leg; the aarch64/qemu leg runs
# only where that toolchain is present locally (real ARM64 lockstep parity is
# covered natively by the Apple-silicon macos CI). Exits non-zero if the guard
# trips or any two builds disagree.

set -eu
cd "$(dirname "$0")/.."

echo "== guard: no direct libm transcendentals in the sim path =="
sh tools/check-detmath.sh
echo

SRC="src/sim/detmath.cpp tools/detmath_test.cpp"
FLAGS="-std=c++20 -ffp-contract=off -Isrc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

GOLDEN=""
fail=0

record() {  # $1=label  $2=output-of-a-run
    h=$(printf '%s\n' "$2" | awk '/detmath golden/{print $3}')
    if [ -z "$h" ]; then echo "  $1: NO HASH"; fail=1; return; fi
    [ -z "$GOLDEN" ] && GOLDEN="$h"
    if [ "$h" = "$GOLDEN" ]; then
        echo "  $1: $h  OK"
    else
        echo "  $1: $h  MISMATCH (expected $GOLDEN)"; fail=1
    fi
}

build_run() {  # $1=label  $2=compiler  $3="extra flags"  $4=runner (or "")
    bin="$WORK/$(printf '%s' "$1" | tr -c 'A-Za-z0-9' _)"
    if ! $2 $FLAGS $3 $SRC -o "$bin" 2>"$WORK/err"; then
        echo "  $1: SKIP (build failed -- missing target libc/headers?)"
        return 0
    fi
    record "$1" "$($4 "$bin" --hash)"
}

echo "== golden-hash agreement across builds =="
arch=$(uname -m)
for cc in g++ clang++; do
    if ! command -v "$cc" >/dev/null 2>&1; then echo "  $cc: SKIP (not installed)"; continue; fi
    for opt in -O0 -O2 -O3; do build_run "$arch $cc $opt" "$cc" "$opt" ""; done
done

if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 && command -v qemu-aarch64-static >/dev/null 2>&1; then
    for opt in -O0 -O2 -O3; do
        build_run "aarch64 g++ $opt" aarch64-linux-gnu-g++ "$opt -static" qemu-aarch64-static
    done
else
    echo "  aarch64: SKIP (need g++-aarch64-linux-gnu + qemu-user-static)"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "DETERMINISM CHECK FAILED"
    exit 1
fi
echo "DETERMINISM OK -- every build agrees on golden $GOLDEN"
