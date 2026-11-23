#!/usr/bin/env python3
"""aura_bench_ab.py OUTDIR -- summarise tools/aura_bench_ab.sh: per workload and build, the median over rounds of each
run's per-pass mean/median/p95/p99/max/min/p10 (us), the per-round ratios cand/base - 1 (base and candidate ran back
to back, ABBA), the retained aura scratch, the work counters, and whether the buff digests agree (they must)."""
import glob, os, statistics as st, sys

KEYS = ('mean_us', 'median_us', 'p95_us', 'p99_us', 'max_us', 'min_us', 'p10_us')


def load(path):
    d = {}
    for line in open(path):
        if line.startswith(('AURABENCH', 'AURATIME', 'AURAHASH')):
            for kv in line.split()[1:]:
                k, v = kv.split('=', 1)
                d[k] = v
    return d


def main():
    out = sys.argv[1]
    names = sorted({os.path.basename(p).split('.')[0] for p in glob.glob(os.path.join(out, 'runs', '*.*.*'))})
    for name in names:
        runs = {'base': {}, 'cand': {}}
        for p in glob.glob(os.path.join(out, 'runs', name + '.*.*')):
            role, r = p.split('.')[-2], int(p.split('.')[-1])
            d = load(p)
            if 'mean_us' in d:
                runs[role][r] = d
        rounds = sorted(set(runs['base']) & set(runs['cand']))
        if not rounds:
            print(f'## {name}: no complete pairs\n')
            continue
        b0 = runs['base'][rounds[0]]
        print(f"## {name}: {len(rounds)} rounds, workers={b0['workers']} units={b0['units']} emitters={b0['emitters']} "
              f"entries={b0['entries']} cells={b0['cells']} pairs={b0['pairs']} hits={b0['hits']} recipients={b0['recipients']}")
        for k in KEYS:
            bs = [float(runs['base'][r][k]) for r in rounds]
            cs = [float(runs['cand'][r][k]) for r in rounds]
            print(f"  {k:10} base {st.median(bs):10.1f}  cand {st.median(cs):10.1f}  {100 * (st.median(cs) / st.median(bs) - 1):+7.1f}%")
        for k in ('mean_us', 'median_us', 'p10_us'):
            pr = [100 * (float(runs['cand'][r][k]) / float(runs['base'][r][k]) - 1) for r in rounds]
            print(f"  per-round {k[:-3]:6} ratio %: median {st.median(pr):+.1f}, range {min(pr):+.1f}..{max(pr):+.1f}")
        print(f"  retained aura scratch bytes: base {b0['retained_bytes']} cand {runs['cand'][rounds[0]]['retained_bytes']}")
        hb = {runs['base'][r]['buffs'] for r in rounds}
        hc = {runs['cand'][r]['buffs'] for r in rounds}
        ok = hb == hc and len(hb) == 1
        print(f"  buff digest: base {','.join(sorted(hb))} cand {','.join(sorted(hc))} ({'identical' if ok else 'DIFFERENT'})\n")


if __name__ == '__main__':
    main()
