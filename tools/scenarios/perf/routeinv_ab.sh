#!/bin/bash
# routeinv_ab.sh BUILD_A BUILD_B SCN TICKS OUT [ARGS_A] [ARGS_B] -- perf follow-up I timing pairs (screening).
#
# Run it on the reserved timing cores, through their lock:
#   /home/pocket_geek/tak-tmp/tools/timing.sh tools/scenarios/perf/routeinv_ab.sh BASE CAND SCN 1000 OUT
# A on cores 0-1 and B on 2-3 concurrently, then swapped; ROUNDS rounds (env, default 1). Each run is
#   legion_scenario SCN --mode legion --offsets 0 --workers --ticks TICKS --no-observer --json $EXTRA ARGS_x
# with TAK_PHASE=1 TAK_PHASE_MS=0 (one SIMPHASE line a tick) and TAK_LPROBE=100 (the LPROBE line: syncStatic's summed
# wall time sync_ms in every build, staticChanged's own changed_ms in builds with the route index). SCN is a plain .scn
# (zcat a .scn.gz first); EXTRA adds options to both arms (a situation: "--data DIR --catchup 60"); ARGS_B
# "--no-route-index" runs the candidate's walk of every group for an A/B inside one binary. Scratch HOME / XDG per run.
# Summarise with: python3 tools/scenarios/perf/routeinv_ab.py OUT WARM
set -u
A=$1; B=$2; SCN=$3; TICKS=$4; OUT=$5; XA=${6:-}; XB=${7:-}; ROUNDS=${ROUNDS:-1}
mkdir -p "$OUT"
run() {   # build cores args log
    local x; x=$(mktemp -d "${TMPDIR:-/tmp}/routeinv-ab.XXXX")
    env HOME="$x" XDG_DATA_HOME="$x/data" XDG_CONFIG_HOME="$x/config" TAK_PHASE=1 TAK_PHASE_MS=0 TAK_LPROBE=100 \
        taskset -c "$2" "$1/legion_scenario" "$SCN" --mode legion --offsets 0 --workers --ticks "$TICKS" --no-observer \
        --json ${EXTRA:-} $3 > "$4.json" 2> "$4.err"
    echo "exit $?" >> "$4.err"
    rm -rf "$x"
}
for r in $(seq 1 "$ROUNDS"); do
    run "$A" 0-1 "$XA" "$OUT/r$r-A-01" & run "$B" 2-3 "$XB" "$OUT/r$r-B-23" & wait
    run "$B" 0-1 "$XB" "$OUT/r$r-B-01" & run "$A" 2-3 "$XA" "$OUT/r$r-A-23" & wait
done
