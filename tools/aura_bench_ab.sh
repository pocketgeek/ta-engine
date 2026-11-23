#!/usr/bin/env bash
# aura_bench_ab.sh --base BUILD --cand BUILD [--rounds N] [--out DIR] [--data DIR] [--set NAME]
#   -- A/B rounds of tools/aura_bench (perf follow-up L): per workload, base and candidate alternate ABBA
#   on the reserved timing cores (/home/pocket_geek/tak-tmp/tools/timing.sh: one lock hold for the set), then
#   tools/aura_bench_ab.py prints per build the median over rounds of each run's mean/median/p95/p99/max/min/p10,
#   the per-round ratios, the retained scratch and whether the buff digests agree (they must: the pass is exact).
# BUILD is a Debug -O2 -g build dir holding aura_bench (both trees need the aura_bench commits).
# Sets (--set): screen (default) = the workloads of the L screening, quick = dense + R-2h only.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BASE= CAND= ROUNDS=4 OUT= SET=screen DATA=${TAK_DATA:-$HOME/TAK/assets/game}
TIMING=${TAK_TIMING_SH:-/home/pocket_geek/tak-tmp/tools/timing.sh}
while [ $# -gt 0 ]; do case $1 in
  --base) BASE=$2; shift 2;; --cand) CAND=$2; shift 2;; --rounds) ROUNDS=$2; shift 2;; --out) OUT=$2; shift 2;;
  --data) DATA=$2; shift 2;; --set) SET=$2; shift 2;; --inner) INNER=1; shift;;
  *) echo "aura_bench_ab.sh: unknown option $1" >&2; exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$CAND" ] || { sed -n 2,9p "$0" >&2; exit 2; }
OUT=${OUT:-${TMPDIR:-/tmp}/aura-ab-$(date +%m%d-%H%M)}
SCN=$OUT/scn
# name|aura_bench arguments
W=( "dense12k-1000d-w3|--layout dense --units 12000 --emitters 1000 --debuff --workers 3 --passes 200 --warm 10"
    "dense12k-1000d-w0|--layout dense --units 12000 --emitters 1000 --debuff --workers 0 --passes 60 --warm 5"
    "R2h-w3|--layout scn --scn $SCN/R-2h-1800.scn --data $DATA --workers 3 --passes 400 --warm 20"
    "R2h-w0|--layout scn --scn $SCN/R-2h-1800.scn --data $DATA --workers 0 --passes 300 --warm 20" )
[ "$SET" = screen ] && W+=(
    "L2h-w3|--layout scn --scn $SCN/L-2h-1800.scn --data $DATA --workers 3 --passes 400 --warm 20"
    "L2h-w0|--layout scn --scn $SCN/L-2h-1800.scn --data $DATA --workers 0 --passes 300 --warm 20"
    "sparse12k-200-w3|--layout sparse --units 12000 --emitters 200 --workers 3 --passes 400 --warm 20"
    "sparse12k-200-w0|--layout sparse --units 12000 --emitters 200 --workers 0 --passes 300 --warm 20"
    "dense4k-100-w3|--layout dense --units 4000 --emitters 100 --workers 3 --passes 600 --warm 20"
    "small300-5-w0|--layout sparse --units 300 --emitters 5 --workers 0 --passes 2000 --warm 50"
    "dense2000-40-w0|--layout dense --units 2000 --emitters 40 --workers 0 --passes 1000 --warm 50"
    "relax-R2h-w3|--layout scn --scn $SCN/R-2h-1800.scn --data $DATA --workers 3 --passes 400 --warm 20 --relax-only" )
if [ -z "${INNER:-}" ]; then
  mkdir -p "$SCN" "$OUT/runs"
  for s in R-2h-1800 L-2h-1800; do [ -s "$SCN/$s.scn" ] || zcat "$HERE/scenarios/we-timing/$s.scn.gz" > "$SCN/$s.scn"; done
  echo "aura_bench_ab: base=$BASE cand=$CAND rounds=$ROUNDS set=$SET out=$OUT"
  "$TIMING" bash "$0" --inner --base "$BASE" --cand "$CAND" --rounds "$ROUNDS" --out "$OUT" --data "$DATA" --set "$SET"
  python3 "$HERE/aura_bench_ab.py" "$OUT" | tee "$OUT/report.txt"
  exit 0
fi
# ---- inner: on the timing cores, holding the lock ----
for w in "${W[@]}"; do
  name=${w%%|*}; read -r -a args <<< "${w#*|}"
  for r in $(seq 1 "$ROUNDS"); do
    if [ $((r % 2)) = 1 ]; then order="base cand"; else order="cand base"; fi
    for role in $order; do
      b=$BASE; [ $role = cand ] && b=$CAND
      "$b/aura_bench" "${args[@]}" > "$OUT/runs/$name.$role.$r" 2>&1 || echo "run failed: $name $role $r" >&2
    done
  done
  echo "$name done"
done
