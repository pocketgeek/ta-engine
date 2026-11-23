#!/usr/bin/env bash
# legion_staticsync_ab.sh --base BUILD --cand BUILD [--rounds N] [--out DIR] [-- BENCH ARGS...]
#
# Perf follow-up H: A/B timing of tools/legion_staticsync_bench (a base-heavy world, several movement
# planes, small localized static changes per step). Per round two swapped concurrent pairs, base on
# core 2 + candidate on core 3, then swapped, all inside one hold of the timing lock
# (/home/pocket_geek/tak-tmp/tools/timing.sh, cores 0-3). BUILD holds legion_staticsync_bench of an
# -O2 tree (Debug -O2 -g); both trees need the bench (cherry-pick its commit onto a base without it).
# Default workloads (each with the given bench args appended):
#   w3000       --structures 3000                         (the default 512x512 map, 6 planes, 4 changes/step)
#   w2000c12p8  --structures 2000 --changes 12 --planes 8 (heavier churn, more planes)
#   w300        --structures 300
#   w50         --structures 50                           (below the index's 64-structure gate: control)
# Prints per workload and build the median over runs of each run's sync/tick mean, median, p95, p99,
# the max, the per-pair cand/base ratio of the sync and tick means, and the final hashes (must agree).
set -euo pipefail
BASE= CAND= ROUNDS=3 OUT= EXTRA=()
while [ $# -gt 0 ]; do case $1 in
  --base) BASE=$2; shift 2;; --cand) CAND=$2; shift 2;; --rounds) ROUNDS=$2; shift 2;; --out) OUT=$2; shift 2;;
  --) shift; EXTRA=("$@"); break;; *) echo "legion_staticsync_ab.sh: unknown option $1" >&2; exit 2;; esac; done
[ -n "$BASE" ] && [ -n "$CAND" ] || { sed -n 2,16p "$0" >&2; exit 2; }
for d in "$BASE" "$CAND"; do [ -x "$d/legion_staticsync_bench" ] || { echo "$d/legion_staticsync_bench missing" >&2; exit 2; }; done
OUT=${OUT:-${TMPDIR:-/tmp}/staticsync-ab-$(date +%m%d-%H%M)}
mkdir -p "$OUT"
TIMING=${TAK_TIMING_SH:-/home/pocket_geek/tak-tmp/tools/timing.sh}
if [ -z "${STATICSYNC_INNER:-}" ]; then
  "$TIMING" env STATICSYNC_INNER=1 bash "$0" --base "$BASE" --cand "$CAND" --rounds "$ROUNDS" --out "$OUT" -- "${EXTRA[@]}"
  python3 - "$OUT" <<'EOF'
import glob,re,statistics as st,sys
out=sys.argv[1]
names=sorted({re.match(r'(.*)-r\d+-',f.split('/')[-1]).group(1) for f in glob.glob(out+'/*.txt')})
for w in names:
    runs={}
    for f in sorted(glob.glob(f'{out}/{w}-r*-*.txt')):
        r,b,c=re.match(r'.*-r(\d+)-(base|cand)-c(\d)\.txt',f).groups()
        t=open(f).read()
        def get(n):
            m=re.search(rf'BENCH {n} n=\d+ mean=([\d.]+) median=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+)',t)
            return [float(x) for x in m.groups()]
        runs.setdefault(b,[]).append((r,c,get('sync_ms'),get('tick_ms'),re.search(r'hash=(\w+)',t).group(1)))
    print(f'== {w}')
    for b in ('base','cand'):
        rs=runs.get(b,[])
        for k,n in ((2,'sync'),(3,'tick')):
            c=list(zip(*[x[k] for x in rs]))
            print(f'  {b} {n}_ms: runs={len(rs)} mean={st.median(c[0]):.4f} median={st.median(c[1]):.4f} p95={st.median(c[2]):.4f} p99={st.median(c[3]):.4f} max={max(c[4]):.4f}')
        print(f'  {b} hash={",".join(sorted({x[4] for x in rs}))}')
    base={(x[0],x[1]):x for x in runs.get('base',[])}
    for k,n in ((2,'sync'),(3,'tick')):
        r=[x[k][0]/base[(x[0],'2' if x[1]=='3' else '3')][k][0] for x in runs.get('cand',[]) if (x[0],'2' if x[1]=='3' else '3') in base]
        if r: print(f'  pair cand/base {n} mean: median={st.median(r):.3f} range={min(r):.3f}..{max(r):.3f}')
EOF
  exit 0
fi
# ---- inner: on the timing cores, holding the lock ----
pair() {  # pair <name> <bench args...>
  local n=$1; shift
  for r in $(seq 1 "$ROUNDS"); do
    taskset -c 2 "$BASE/legion_staticsync_bench" "$@" > "$OUT/$n-r$r-base-c2.txt" 2>&1 &
    taskset -c 3 "$CAND/legion_staticsync_bench" "$@" > "$OUT/$n-r$r-cand-c3.txt" 2>&1 & wait
    taskset -c 3 "$BASE/legion_staticsync_bench" "$@" > "$OUT/$n-r$r-base-c3.txt" 2>&1 &
    taskset -c 2 "$CAND/legion_staticsync_bench" "$@" > "$OUT/$n-r$r-cand-c2.txt" 2>&1 & wait
  done
}
pair w3000 --structures 3000 --steps 300 "${EXTRA[@]}"
pair w2000c12p8 --structures 2000 --changes 12 --planes 8 --steps 300 "${EXTRA[@]}"
pair w300 --structures 300 --steps 300 "${EXTRA[@]}"
pair w50 --structures 50 --steps 300 "${EXTRA[@]}"
