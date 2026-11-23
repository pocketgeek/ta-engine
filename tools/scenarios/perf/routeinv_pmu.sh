#!/bin/bash
# routeinv_pmu.sh BASE CAND SCN TICKS OUT -- perf follow-up I work measurement: user-mode instructions and cycles per
# SIMPHASE phase (TAK_PMU=1: one SIMPMU line a tick; debug builds, Linux), --serial, for the base build, the candidate
# and the candidate with --no-route-index, run concurrently on P-cores (CORES, default 4-7: keep every run on one core
# type). Instruction counts hardly depend on load; cycles do (use the timing pairs for time). EXTRA adds options to
# every run (a situation: "--data DIR --catchup 60"). Summarise with: python3 tools/scenarios/perf/routeinv_pmu.py OUT WARM
set -u
BASE=$1; CAND=$2; SCN=$3; TICKS=$4; OUT=$5; CORES=${CORES:-4-7}
mkdir -p "$OUT"
run() {   # build args name
    local x; x=$(mktemp -d "${TMPDIR:-/tmp}/routeinv-pmu.XXXX")
    env HOME="$x" XDG_DATA_HOME="$x/data" XDG_CONFIG_HOME="$x/config" TAK_PHASE=1 TAK_PHASE_MS=0 TAK_PMU=1 \
        taskset -c "$CORES" "$1/legion_scenario" "$SCN" --mode legion --offsets 0 --serial --ticks "$TICKS" \
        --no-observer --json ${EXTRA:-} $2 > "$OUT/$3.json" 2> "$OUT/$3.err"
    grep -E "^SIMPMU" "$OUT/$3.err" > "$OUT/$3.pmu"
    grep -vE "^SIMPMU|^SIMPHASE" "$OUT/$3.err" > "$OUT/$3.rest"
    rm -f "$OUT/$3.err"; rm -rf "$x"
}
run "$BASE" "" base & run "$CAND" "" cand & run "$CAND" "--no-route-index" candoff & wait
