#!/bin/sh
# Determinism guard for the simulation. The deterministic sim path must not call
# libm transcendentals directly: sin/cos/tan/atan2/atan/asin/acos/hypot/pow/exp/
# log are NOT correctly-rounded, so each platform's libm may differ in the last
# bit and break cross-build lockstep. Route trig through tak::detmath instead.
# (std::sqrt and std::fmod are fine -- correctly rounded / exact.)
# See docs/detmath-scope.md.
#
# Run from anywhere; exits non-zero if a violation is found. Wire into CI.

cd "$(dirname "$0")/.." || exit 2

# Files that run inside World::tick / setupMatch / the AI controller (i.e. feed
# the state hash). detmath.{h,cpp} is the one place that may name these.
FILES="src/sim/retailaim.h src/sim/sim.cpp src/sim/sim.h src/sim/matchsetup.cpp src/sim/matchsetup.h src/ai/ai.cpp src/ai/ai.h src/ai/classicbrain.cpp src/ai/classicbrain.h src/ai/classicprofile.cpp src/ai/classicprofile.h src/ai/classictypes.cpp src/ai/classictypes.h src/ai/classicsquads.h src/ai/classicproduction.h src/ai/classicplacement.h"

# Match a call to one of the banned functions (bare or std::-qualified), but not
# a longer identifier that merely ends in the name (e.g. myLog(), ::detmath::sin).
PAT='(^|[^A-Za-z0-9_:])(std::)?(sin|cos|tan|atan2|atan|asin|acos|hypot|pow|exp|log)f?[[:space:]]*\('

# COMMENTS ARE STRIPPED FIRST. The guard matches raw text, so a comment that merely
# DESCRIBES the rule trips it -- which is not hypothetical in a codebase that documents
# its math this heavily: "a computed ring (cx + cos(a)*radius)" took CI red on a commit
# that touched no arithmetic at all. Stripping keeps line numbers (the comment body is
# blanked, the line stays), so reported positions still point at real code.
#
# Both comment forms are handled, and stripping is deliberately conservative: anything
# it cannot classify stays IN the text and is still matched. A guard that fails open
# would be worse than the false positive it replaces.
strip_comments() {
    awk '
    BEGIN { inblk = 0 }
    {
        line = $0; out = ""
        while (length(line) > 0) {
            if (inblk) {
                p = index(line, "*/")
                if (p == 0) { line = ""; break }
                line = substr(line, p + 2); inblk = 0
                continue
            }
            b = index(line, "/*"); l = index(line, "//")
            if (l > 0 && (b == 0 || l < b)) { line = substr(line, 1, l - 1); break }
            if (b == 0) break
            out = out substr(line, 1, b - 1); line = substr(line, b + 2); inblk = 1
        }
        print out line
    }' "$1"
}

hits=""
for f in $FILES; do
    [ -f "$f" ] || continue
    h=$(strip_comments "$f" | grep -nE "$PAT" | grep -v 'detmath::' | sed "s|^|$f:|")
    [ -n "$h" ] && hits="$hits$h
"
done
hits=$(printf '%s' "$hits" | sed '/^$/d')
if [ -n "$hits" ]; then
    echo "ERROR: the sim path calls a libm transcendental directly:"
    echo "$hits"
    echo
    echo "These are not correctly-rounded and break cross-build lockstep."
    echo "Route through tak::detmath (src/sim/detmath.h). See docs/detmath-scope.md."
    exit 1
fi
echo "OK: no direct libm transcendentals in the sim path."

# Portability, checked here because this is the guard CI runs first: Apple's libc++
# (the macOS CI toolchain) has no std::atomic_ref, and a newer local Xcode does, so
# nothing short of CI caught it -- it broke the macOS build twice. Use
# __atomic_store_n / __atomic_load_n (see World::markViewDirty or rebuildSpatialParallel).
ref=""
for f in $(find src tools -name '*.cpp' -o -name '*.h' | sort); do
    h=$(strip_comments "$f" | grep -n 'atomic_ref' | sed "s|^|$f:|")
    [ -n "$h" ] && ref="$ref$h
"
done
ref=$(printf '%s' "$ref" | sed '/^$/d')
if [ -n "$ref" ]; then
    echo "ERROR: std::atomic_ref does not exist in Apple's libc++ (macOS build):"
    echo "$ref"
    echo "Use __atomic_store_n / __atomic_load_n instead."
    exit 1
fi
echo "OK: no std::atomic_ref."
