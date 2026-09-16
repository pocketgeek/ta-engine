#!/bin/sh
# Determinism guard for the simulation. The deterministic sim path must not call
# libm transcendentals directly: sin/cos/tan/atan2/atan/asin/acos/hypot/pow/exp/
# log are NOT correctly-rounded, so each platform's libm may differ in the last
# bit and break cross-build lockstep. Route trig through ta::detmath instead.
# (std::sqrt and std::fmod are fine -- correctly rounded / exact.)
# See docs/detmath-scope.md.
#
# Run from anywhere; exits non-zero if a violation is found. Wire into CI.

cd "$(dirname "$0")/.." || exit 2

# Files that run inside World::tick / setupMatch / the AI controller (i.e. feed
# the state hash). detmath.{h,cpp} is the one place that may name these.
FILES="src/sim/sim.cpp src/sim/sim.h src/sim/matchsetup.cpp src/sim/matchsetup.h src/ai/ai.cpp src/ai/ai.h"

# Match a call to one of the banned functions (bare or std::-qualified), but not
# a longer identifier that merely ends in the name (e.g. myLog(), ::detmath::sin).
PAT='(^|[^A-Za-z0-9_:])(std::)?(sin|cos|tan|atan2|atan|asin|acos|hypot|pow|exp|log)f?[[:space:]]*\('

hits=$(grep -nE "$PAT" $FILES 2>/dev/null | grep -v 'detmath::')
if [ -n "$hits" ]; then
    echo "ERROR: the sim path calls a libm transcendental directly:"
    echo "$hits"
    echo
    echo "These are not correctly-rounded and break cross-build lockstep."
    echo "Route through ta::detmath (src/sim/detmath.h). See docs/detmath-scope.md."
    exit 1
fi
echo "OK: no direct libm transcendentals in the sim path."
