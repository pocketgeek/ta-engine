#!/usr/bin/env bash
# we_timing.sh --base BUILD --cand BUILD [options]   -- the fast WE A/B timing round (docs/development.md "WE timing")
#
# Replaces "replay R-2h / L-2h from tick 0, twice, swapped" (about 70 min) with swapped concurrent pairs that start
# from harvested situations (tools/scenarios/we-timing/*.scn.gz, cut by tools/we_timing_harvest.sh) just before the
# measured window and simulate only warm-up + window. A round is about 15 minutes. It is TIMING only: identity (hashes,
# replays, crowdbench rows) still comes from the full replays and tools/legion_identity.sh.
#
# BUILD is a build dir holding legion_scenario of an -O2 (Debug -O2 -g) tree (TAK_PHASE is debug-only); BOTH trees need the tool
# files of this commit (map named, TAK_SITUATION_CMDS): cherry-pick the commit onto a base/candidate that predates it.
#   --base DIR --cand DIR   the two build dirs (required)
#   --sits LIST             comma list of situations (default: every tools/scenarios/we-timing/*.scn.gz)
#   --sitdir DIR            where the *.scn.gz live (default tools/scenarios/we-timing)
#   --replays DIR           exact mode: instead of rebuilding the situation, play DIR/<R-2h|L-2h|R-bench|L-bench>.takrep
#                           (matching each build's protocol) from tick 0, keep the SIMPHASE lines from the harvest tick on and
#                           stop at harvest + warm + ticks. Needs a DEBUG/-O2 takclient in each build dir. Same report, no
#                           situation fidelity limits, but ~2x the wall (a replay is simulated from tick 0)
#   --rounds N              swapped-pair rounds (default 2)
#   --warm N                warm-up ticks, dropped (default 1000: with --catchup the rebuilt world has settled
#                           by ~700 ticks; see docs/development.md)
#   --ticks N               ticks measured after the warm-up (default 1000)
#   --catchup N             ticks of unlimited path budget at the start, draining the order backlog a situation re-issues at
#                           tick 0 (default 60; 0 = off)
#   --counters              also collect TAK_SIMSTATS work counters (adds instrument cost: not for ms comparisons)
#   --data DIR              game install (default $TAK_DATA or ~/TAK/assets/game)
#   --out DIR               logs + report (default $TMPDIR/we-timing-<date>)
# Every timed run goes through /home/pocket_geek/tak-tmp/tools/timing.sh (flock; cores 0-3): one pair on 0-1 + 2-3.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BASE= CAND= SITS= SITDIR= REPLAYS= ROUNDS=2 WARM=1000 TICKS=1000 CATCHUP=60 COUNTERS= INNER=
DATA=${TAK_DATA:-$HOME/TAK/assets/game}; OUT=
TIMING=${TAK_TIMING_SH:-/home/pocket_geek/tak-tmp/tools/timing.sh}
while [ $# -gt 0 ]; do case $1 in
  --base) BASE=$2; shift 2;; --cand) CAND=$2; shift 2;; --sits) SITS=$2; shift 2;; --sitdir) SITDIR=$2; shift 2;; --replays) REPLAYS=$2; shift 2;; --rounds) ROUNDS=$2; shift 2;;
  --warm) WARM=$2; shift 2;; --ticks) TICKS=$2; shift 2;; --counters) COUNTERS=1; shift;; --catchup) CATCHUP=$2; shift 2;; --data) DATA=$2; shift 2;;
  --out) OUT=$2; shift 2;; --inner) INNER=1; shift;; *) echo "we_timing.sh: unknown option $1" >&2; exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$CAND" ] || { sed -n 2,22p "$0" >&2; exit 2; }
for d in "$BASE" "$CAND"; do
  if [ -n "$REPLAYS" ]; then [ -x "$d/takclient" ] || { echo "we_timing.sh: $d/takclient missing" >&2; exit 2; }
  else [ -x "$d/legion_scenario" ] || { echo "we_timing.sh: $d/legion_scenario missing" >&2; exit 2; }; fi
done
OUT=${OUT:-${TMPDIR:-/tmp}/we-timing-$(date +%m%d-%H%M)}
SITDIR=${SITDIR:-$HERE/scenarios/we-timing}
mkdir -p "$OUT/scn" "$OUT/runs"

# Situations, decompressed once.
if [ -z "$INNER" ]; then
  if [ -z "$SITS" ]; then SITS=$(cd "$SITDIR" && ls *.scn.gz | sed 's/\.scn\.gz$//' | paste -sd,); fi
  for s in ${SITS//,/ }; do
    # Fail on a bad name here: a wrong one used to run every round in ~0 s and report nothing.
    if [ -n "$REPLAYS" ]; then
      [[ $s =~ ^.+-[0-9]+$ ]] || { echo "we_timing.sh: --replays situation '$s' must be <replay>-<harvest tick>, e.g. R-2h-1800" >&2; exit 2; }
      [ -s "$REPLAYS/${s%-*}.takrep" ] || { echo "we_timing.sh: no replay $REPLAYS/${s%-*}.takrep for situation '$s'" >&2; exit 2; }
    else
      [ -s "$SITDIR/$s.scn.gz" ] || { echo "we_timing.sh: no situation $SITDIR/$s.scn.gz" >&2; exit 2; }
    fi
    [ -n "$REPLAYS" ] || [ -s "$OUT/scn/$s.scn" ] || zcat "$SITDIR/$s.scn.gz" > "$OUT/scn/$s.scn"
  done
  echo "we_timing: base=$BASE cand=$CAND sits=$SITS rounds=$ROUNDS warm=$WARM out=$OUT"
  args=(--inner --base "$BASE" --cand "$CAND" --sits "$SITS" --rounds "$ROUNDS" --warm "$WARM" --catchup "$CATCHUP" --data "$DATA" --out "$OUT")
  [ -n "$REPLAYS" ] && args+=(--replays "$REPLAYS")
  [ -n "$TICKS" ] && args+=(--ticks "$TICKS"); [ -n "$COUNTERS" ] && args+=(--counters)
  start=$(date +%s)
  "$TIMING" bash "$0" "${args[@]}"
  echo "we_timing: wall $(( $(date +%s) - start )) s (including any wait for the timing cores)"
  # A run that produced no SIMPHASE lines (most often a replay recorded under another protocol,
  # which takclient refuses) must not pass as a timing result.
  empty=$(find "$OUT/runs" -type f ! -name '*.*' -empty | sort)
  if [ -n "$empty" ]; then
    echo "we_timing.sh: $(echo "$empty" | wc -l) run(s) recorded no SIMPHASE lines -- replay protocol must match each build's (legion_identity.py patch-replay); see the .out next to:" >&2
    echo "$empty" | head -4 >&2; exit 1
  fi
  python3 "$HERE/we_timing.py" "$OUT" --warm "$WARM" | tee "$OUT/report.txt"
  exit 0
fi

# ---- inner: already on the timing cores, holding the lock ----
one() {   # one <build> <cores> <sit> <log>
  local mode=R; [[ $3 == L-* ]] && mode=L
  local ticks=$TICKS
  local t0=$(date +%s.%N)
  if [ -n "$REPLAYS" ]; then   # truncated exact replay: sit name R-2h-1800 = replay R-2h, harvest tick 1800
    local rep=${3%-*} from=${3##*-} X; X=$(mktemp -d)
    env XDG_DATA_HOME=$X TAK_PHASE=1 TAK_PHASE_MS=0 TAK_REPLAY_VERIFY=1 TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy \
        taskset -c "$2" "$1/takclient" replay "$REPLAYS/$rep.takrep" --data "$DATA" 2>&1 \
        | grep "^SIMPHASE" | head -n $((from + WARM + ticks)) | tail -n +$((from + 1)) > "$4" || true
    rm -rf "$X"; echo "replay $rep" > "$4.out"
  else
    env TAK_PHASE=1 TAK_PHASE_MS=0 ${COUNTERS:+TAK_SIMSTATS=1} taskset -c "$2" "$1/legion_scenario" "$OUT/scn/$3.scn" \
        --mode $([ $mode = R ] && echo retail || echo legion) --data "$DATA" --ticks $((WARM + ticks)) --offsets 0 \
        --no-observer --workers --catchup "$CATCHUP" 2> "$4.err" > "$4.out" || { echo "run failed: $4 ($(tail -1 "$4.err"))" >&2; return 0; }
    grep -E "^SIMPHASE|^SIMSTATS" "$4.err" > "$4" || true
    rm -f "$4.err"
  fi
  echo "wall $(echo "$(date +%s.%N) - $t0" | bc)" >> "$4.out"
}
for r in $(seq 1 "$ROUNDS"); do
  for s in ${SITS//,/ }; do
    p="$OUT/runs/r$r-$s"
    one "$BASE" 0-1 "$s" "$p-base-01" & one "$CAND" 2-3 "$s" "$p-cand-23" & wait
    one "$CAND" 0-1 "$s" "$p-cand-01" & one "$BASE" 2-3 "$s" "$p-base-23" & wait
    echo "round $r $s done"
  done
done
