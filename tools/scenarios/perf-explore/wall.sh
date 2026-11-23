#!/usr/bin/env bash
# wall.sh --base BUILD --cand BUILD [--sits LIST] [--ticks N] [--reps N] [--core C] [--data DIR] [--out DIR]
#
# Perf follow-up D, the map-sized cost alone: the whole-run time of legion_scenario (World::tick plus the
# feed and the 100-tick state digests; no TAK_PHASE, so no per-tick timer) of the scenarios here, base and
# candidate alternating on ONE timing core (taskset -c C inside /home/pocket_geek/tak-tmp/tools/timing.sh),
# REPS runs each. Prints per scenario and build the median and range of the run times and of ms per tick,
# and (base - cand) / ticks: the per-tick saving, resolved far below SIMPHASE's 0.01 ms step.
#   --sits LIST (default the four explore-idle-*)  --ticks N (default 3000)
#   --reps N (default 5)  --core C (default 2)  --data DIR (default $TAK_DATA or ~/TAK/assets/game)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TIMING=${TAK_TIMING_SH:-/home/pocket_geek/tak-tmp/tools/timing.sh}
BASE= CAND= SITS=explore-idle-256,explore-idle-ulasem,explore-idle-1024,explore-idle-2048 TICKS=3000 REPS=5 CORE=2
DATA=${TAK_DATA:-$HOME/TAK/assets/game} OUT= INNER=
while [ $# -gt 0 ]; do case $1 in
  --base) BASE=$2; shift 2;; --cand) CAND=$2; shift 2;; --sits) SITS=$2; shift 2;; --ticks) TICKS=$2; shift 2;;
  --reps) REPS=$2; shift 2;; --core) CORE=$2; shift 2;; --data) DATA=$2; shift 2;; --out) OUT=$2; shift 2;;
  --inner) INNER=1; shift;; *) echo "wall.sh: unknown option $1" >&2; exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$CAND" ] || { sed -n 2,11p "$0" >&2; exit 2; }
if [ -z "$INNER" ]; then
  OUT=${OUT:-${TMPDIR:-/tmp}/explore-wall-$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
  "$TIMING" bash "$0" --inner --base "$BASE" --cand "$CAND" --sits "$SITS" --ticks "$TICKS" --reps "$REPS" \
      --core "$CORE" --data "$DATA" --out "$OUT"
  python3 - "$OUT" "$TICKS" <<'PY'
import glob, os, re, statistics as st, sys
out, ticks = sys.argv[1], int(sys.argv[2])
runs = {}
for f in sorted(glob.glob(os.path.join(out, '*.err'))):
    m = re.match(r'(.+)-(base|cand)-(\d+)\.err$', os.path.basename(f))
    t = [float(x) for x in re.findall(r' (\d+(?:\.\d+)?) ms tick_max_ms', open(f).read())]
    if m and t:
        runs.setdefault(m.group(1), {}).setdefault(m.group(2), []).append(t[0])
for sit, r in sorted(runs.items()):
    print(f"## {sit}: {ticks} ticks, {len(r.get('base', []))}+{len(r.get('cand', []))} runs")
    for role in ('base', 'cand'):
        xs = r.get(role, [])
        if xs:
            print(f"  {role}: run ms median {st.median(xs):9.1f} (range {min(xs):.1f}..{max(xs):.1f}), "
                  f"per tick {st.median(xs) / ticks * 1000:8.2f} us")
    if r.get('base') and r.get('cand'):
        d = (st.median(r['base']) - st.median(r['cand'])) / ticks * 1000
        print(f"  saved per tick (median base - median cand): {d:.2f} us")
PY
  exit 0
fi
for s in ${SITS//,/ }; do
  for i in $(seq 1 "$REPS"); do
    if [ $((i % 2)) = 1 ]; then order="base cand"; else order="cand base"; fi
    for role in $order; do
      b=$BASE; [ $role = cand ] && b=$CAND
      taskset -c "$CORE" "$b/legion_scenario" "$HERE/$s.scn" --mode retail --data "$DATA" --ticks "$TICKS" \
          --offsets 0 --no-observer --serial > "$OUT/$s-$role-$i.out" 2> "$OUT/$s-$role-$i.err" || true
    done
  done
done
