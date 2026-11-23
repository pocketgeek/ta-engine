#!/usr/bin/env bash
# hidden_fade_bench.sh -- HIDDEN UNITS FADE workloads for tools/render_benchmark.py
# (perf follow-up item F: per-frame cover bookkeeping in findHiddenUnits).
#
#   tools/hidden_fade_bench.sh --a BASE_TAKCLIENT --b CAND_TAKCLIENT --data DIR --output DIR \
#       [--repeats N] [--frames N] [--warmup N] [--cpus LIST] [--capture] [-- more render_benchmark args]
#
# Optimized-Debug clients (render study). FADE is the default HIDDEN UNITS mode of a
# fresh profile, so no setting is passed. Three workloads, each an existing scene plus
# environment overrides (no new scene: historical baselines keep their meaning):
#   fade-forest      `forest`: 300 mixed units in the densest scenery of Sea Dragon Spine
#   fade-forest1200  `forest` with 1200 units 24 px apart: a dense crowd under repeated trees
#   fade-keeps       Ulasem Arena, zoom 1, 1000 units 32 px apart, every eighth a Keep:
#                    infantry crowded behind live building geometry (triangle tests)
# Compare t_hidden_units (render-thread wall of the pass) and the frame times; the
# work counters hidden_cover_visits / hidden_cover_reaches exist only in clients with
# the item-F telemetry (visits = the exact tests the old pass ran, reaches = those run
# now); hidden_cover_tests (sprite/triangle tests) is in every client with measurement B.
# Run timing on the reserved cores through /home/pocket_geek/tak-tmp/tools/timing.sh
# with --cpus 0-3; captures (--capture) are separate runs and need no quiet cores.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
a= b= data= out= repeats=2 frames=600 warmup=150 cpus=0-3 capture=()
while [ $# -gt 0 ]; do
    case $1 in
        --a) a=$2; shift 2;;
        --b) b=$2; shift 2;;
        --data) data=$2; shift 2;;
        --output) out=$2; shift 2;;
        --repeats) repeats=$2; shift 2;;
        --frames) frames=$2; shift 2;;
        --warmup) warmup=$2; shift 2;;
        --cpus) cpus=$2; shift 2;;
        --capture) capture=(--capture); shift;;
        --) shift; break;;
        *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$a" ] && [ -n "$b" ] && [ -n "$data" ] && [ -n "$out" ] || {
    sed -n '2,8p' "$0" >&2; exit 2; }
run() {
    local name=$1 scene=$2; shift 2
    local env=()
    for e in "$@"; do env+=(--env "$e"); done
    python3 "$here/render_benchmark.py" --a "$a" --b "$b" --data "$data" --output "$out/$name" \
        --scenes "$scene" --repeats "$repeats" --frames "$frames" --warmup "$warmup" --cpus "$cpus" \
        "${capture[@]}" "${env[@]}" "${extra[@]}"
}
extra=("$@")
run fade-forest forest
run fade-forest1200 forest TAK_SHADOW_BENCH=1200 TAK_RENDER_SPACING=24
run fade-keeps stationary-mixed TAK_PROFILE_ZOOM=1.0 TAK_SHADOW_BENCH=1000 TAK_RENDER_SPACING=32 \
    TAK_RENDER_ROSTER=arakeep,araarch,araarch,araarch,araarch,araarch,araarch,araarch
