#!/usr/bin/env bash
# run.sh --base BUILD --cand BUILD [--sits LIST] [--rounds N] [--warm N] [--ticks N] [--data DIR] [--out DIR]
#
# Perf follow-up D timing (World::updateExplorationPlane's block summaries): runs the scenarios of this directory
# as tools/we_timing.sh situations -- Retail mode, base and candidate in swapped concurrent pairs on the timing
# cores through /home/pocket_geek/tak-tmp/tools/timing.sh -- and prints, per scenario and build, the per-tick
# distribution (mean, median, p95, p99, max over every measured tick of every run) of SIMPHASE explore= (the
# exploration pass and the view counts) and tick= (the complete World::tick), plus the pair ratios of the means.
# BUILD is a Debug -O2 -g build dir with legion_scenario (TAK_PHASE is debug-only).
#   --sits LIST   comma list of scenario names (default: every *.scn here)
#   --rounds N    swapped-pair rounds (default 2); --warm N dropped warm-up ticks (default 100); --ticks N (default 400)
#   --data DIR    game install (default $TAK_DATA or ~/TAK/assets/game; the flat maps do not read it)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(cd "$HERE/../.." && pwd)
BASE= CAND= SITS= ROUNDS=2 WARM=100 TICKS=400 DATA=${TAK_DATA:-$HOME/TAK/assets/game} OUT=
while [ $# -gt 0 ]; do case $1 in
  --base) BASE=$2; shift 2;; --cand) CAND=$2; shift 2;; --sits) SITS=$2; shift 2;; --rounds) ROUNDS=$2; shift 2;;
  --warm) WARM=$2; shift 2;; --ticks) TICKS=$2; shift 2;; --data) DATA=$2; shift 2;; --out) OUT=$2; shift 2;;
  *) echo "run.sh: unknown option $1" >&2; exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$CAND" ] || { sed -n 2,13p "$0" >&2; exit 2; }
OUT=${OUT:-${TMPDIR:-/tmp}/explore-timing-$(date +%m%d-%H%M%S)}
mkdir -p "$OUT/sits"
[ -n "$SITS" ] || SITS=$(cd "$HERE" && ls *.scn | sed 's/\.scn$//' | paste -sd,)
for s in ${SITS//,/ }; do gzip -c "$HERE/$s.scn" > "$OUT/sits/$s.scn.gz"; done
"$TOOLS/we_timing.sh" --base "$BASE" --cand "$CAND" --sitdir "$OUT/sits" --sits "$SITS" --rounds "$ROUNDS" \
    --warm "$WARM" --ticks "$TICKS" --catchup 0 --data "$DATA" --out "$OUT/we" > "$OUT/we.txt"
python3 "$HERE/phase_stats.py" "$OUT/we" --warm "$WARM" | tee "$OUT/report.txt"
