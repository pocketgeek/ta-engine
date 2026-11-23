#!/bin/bash
# legion_timing.sh -- wall-time measurement for the Legion work, as a REPORT (wall time is never a gate;
# PLAN 3.0). Plays one workload with two builds (A, B) interleaved A B A B ... on one pinned, idle core and
# prints min and median of the per-tick mean and p99 for each build, plus the ratio B/A.
#
#   tools/legion_timing.sh --a BIN_A --b BIN_B [--cpu N] [--runs N] [--replay FILE] [--data DIR]
#                          [--cmd 'TEMPLATE'] [--threshold-ms 4.17]
#   tools/legion_timing.sh --check-idle CPUS      (exit 0 when every listed core is <= 2% busy)
#
# BIN is an optimized-Debug takclient (the `replay` keyword and TAK_PHASE are debug-only). The default
# workload is `BIN replay FILE --data DIR` with TAK_REPLAY_VERIFY=1 (unpaced) and TAK_PHASE=1
# TAK_PHASE_MS=0, one SIMPHASE line per tick; --cmd replaces it ({BIN} stands for the binary, which gets
# the same environment). A replay only plays back exactly on the build that recorded it; after a behaviour
# change it diverges and its commands still play, so the timing remains a realistic workload, not a result.
#
# Rules the audit paid for (IN-08): a core that is busy with anything else spread the audited "+-50%"
# noise; a busy loop on the same core reproduces it column for column, and an idle core gave 2/2/2/2
# ticks over budget. So the script REFUSES a core that was more than 2% busy over the last second, and it
# re-checks before every run. On a shared host the limit can be raised with TAK_MAX_BUSY_PCT=N; the
# numbers are then not comparable with an idle-core run and the report says so. The over-threshold count is ill-conditioned at the knee (the audit's
# 236-644 spread), so it is printed only when the threshold is more than 20% away from the run's p95.
set -u
check_idle() {  # $1 = cpu list (e.g. 22-23 or 5); TAK_MAX_BUSY_PCT overrides the 2% limit (reported when it does)
  python3 - "$1" "${TAK_MAX_BUSY_PCT:-2}" <<'PY'
import sys,time
def parse(spec):
    out=[]
    for part in spec.split(','):
        if '-' in part:
            a,b=part.split('-'); out+=range(int(a),int(b)+1)
        else: out.append(int(part))
    return out
def snap():
    d={}
    for l in open('/proc/stat'):
        f=l.split()
        if f[0].startswith('cpu') and f[0]!='cpu':
            v=list(map(int,f[1:])); d[int(f[0][3:])]=(sum(v[:8]),v[3]+v[4])   # total, idle+iowait
    return d
cpus=parse(sys.argv[1]); limit=float(sys.argv[2]); a=snap(); time.sleep(1.0); b=snap(); bad=False
for c in cpus:
    dt=b[c][0]-a[c][0]; di=b[c][1]-a[c][1]
    busy=100.0*(1-di/dt) if dt else 0.0
    if busy>limit: print(f"core {c} is {busy:.1f}% busy (limit {limit:g}%): refusing to time on it",file=sys.stderr); bad=True
sys.exit(1 if bad else 0)
PY
}
if [ "${1:-}" = "--check-idle" ]; then check_idle "${2:?cpu list}"; exit $?; fi

A=""; B=""; CPU=5; RUNS=4; REPLAY=""; DATA=${TAK_DATA:-assets/game}; CMD=""; THRESH=4.17
while [ $# -gt 0 ]; do
  case "$1" in
    --a) A=$2; shift 2;; --b) B=$2; shift 2;; --cpu) CPU=$2; shift 2;; --runs) RUNS=$2; shift 2;;
    --replay) REPLAY=$2; shift 2;; --data) DATA=$2; shift 2;; --cmd) CMD=$2; shift 2;;
    --threshold-ms) THRESH=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[ -x "$A" ] && [ -x "$B" ] || { echo "usage: $0 --a BIN_A --b BIN_B [options]" >&2; exit 2; }
[ "$RUNS" -ge 4 ] 2>/dev/null || { echo "--runs must be at least 4 (min and median need a sample)" >&2; exit 2; }
if [ -z "$CMD" ]; then
  [ -n "$REPLAY" ] || { echo "give --replay FILE or --cmd" >&2; exit 2; }
  CMD="{BIN} replay $REPLAY --data $DATA"
fi
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
for i in $(seq 1 "$RUNS"); do
  for who in A B; do
    bin=$A; [ $who = B ] && bin=$B
    check_idle "$CPU" || { echo "run $i $who: core $CPU not idle" >&2; exit 3; }
    cmd=${CMD//\{BIN\}/$bin}
    TAK_REPLAY_VERIFY=1 TAK_PHASE=1 TAK_PHASE_MS=0 XDG_DATA_HOME="$work/xdg" TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy \
      taskset -c "$CPU" bash -c "$cmd" > "$work/$who.$i.log" 2>&1
  done
done
python3 - "$work" "$RUNS" "$THRESH" "${TAK_MAX_BUSY_PCT:-2}" <<'PY'
import re,sys,statistics
work,runs,thresh=sys.argv[1],int(sys.argv[2]),float(sys.argv[3])
limit=float(sys.argv[4])
def ticks(path):
    t=[]
    for l in open(path,errors='replace'):
        m=re.search(r'SIMPHASE tick=([\d.]+)ms',l)
        if m: t.append(float(m[1]))
    return t
def p(v,q): s=sorted(v); return s[min(len(s)-1,int(q*len(s)))]
res={}
for who in 'AB':
    rows=[]
    for i in range(1,runs+1):
        t=ticks(f"{work}/{who}.{i}.log")
        if not t: print(f"{who} run {i}: no SIMPHASE lines (is the binary a debug build and the workload valid?)"); sys.exit(2)
        rows.append((statistics.mean(t),p(t,.99),p(t,.95),len(t),t))
    res[who]=rows
def line(who):
    r=res[who]; means=[x[0] for x in r]; p99=[x[1] for x in r]
    out=f"{who}: ticks {r[0][3]}  mean min {min(means):.3f} median {statistics.median(means):.3f} ms | p99 min {min(p99):.2f} median {statistics.median(p99):.2f} ms"
    p95=statistics.median([x[2] for x in r])
    if p95<=0 or abs(thresh-p95)/p95>0.20:
        over=[sum(1 for v in x[4] if v>thresh) for x in r]
        out+=f" | > {thresh} ms: {sorted(over)} (min {min(over)})"
    else:
        out+=f" | ticks over {thresh} ms not reported: the threshold is within 20% of this run's p95 ({p95:.2f} ms)"
    return out
if limit!=2.0: print(f'NOTE: busy limit raised to {limit:g}% (TAK_MAX_BUSY_PCT); not comparable with an idle-core run')
print(line('A')); print(line('B'))
ma=statistics.median([x[0] for x in res['A']]); mb=statistics.median([x[0] for x in res['B']])
print(f"B/A mean (median of runs) {mb/ma:.3f}   [wall time is reported, never gated]")
PY
