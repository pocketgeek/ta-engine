#!/usr/bin/env python3
"""we_timing_dist.py OUTDIR [--warm N] [--phases tick,explore,...] -- per-tick distributions of a we_timing.sh round.

tools/we_timing.py reports each phase's mean ms per tick. This prints, per situation, build and phase, the
distribution over every measured tick of every run of that build (warm-up dropped): n, mean, median, p95, p99,
max, and the ticks over the 30 Hz budget (33.3 ms); then the median over the concurrent pairs of the per-pair
ratio of each statistic (cand/base - 1), the number to quote (pair partners share the host's noise).
"""
import glob, os, re, statistics as st, sys

BUDGET = 1000.0 / 30


def load(path, warm):
    rows = []
    for line in open(path):
        if line.startswith('SIMPHASE'):
            rows.append({k: float(v.rstrip('ms')) for k, v in (kv.split('=') for kv in line.split()[1:])})
    return rows[warm:]


def pick(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def stats(v):
    return {'mean': st.mean(v), 'median': st.median(v), 'p95': pick(v, .95), 'p99': pick(v, .99), 'max': max(v),
            'over': sum(x > BUDGET for x in v)}


def main():
    out = sys.argv[1]
    warm = int(sys.argv[sys.argv.index('--warm') + 1]) if '--warm' in sys.argv else 1000
    phases = (sys.argv[sys.argv.index('--phases') + 1].split(',') if '--phases' in sys.argv
              else ['tick', 'explore', 'sep'])
    runs = {}   # (sit, round, cores) -> {role: rows}
    for f in sorted(glob.glob(os.path.join(out, 'runs', 'r*-*-*-??'))):
        m = re.match(r'r(\d+)-(.+)-(base|cand)-(01|23)$', os.path.basename(f))
        if m:
            rows = load(f, warm)
            if rows:
                runs.setdefault((m[2], m[1], m[4]), {})[m[3]] = rows
    for sit in sorted({k[0] for k in runs}):
        pairs = [v for k, v in sorted(runs.items()) if k[0] == sit and 'base' in v and 'cand' in v]
        if not pairs:
            continue
        print(f"## {sit}: {len(pairs)} concurrent pairs, {len(pairs[0]['base'])} ticks each after {warm} warm-up")
        for ph in phases:
            if ph not in pairs[0]['base'][0]:
                continue
            pooled = {r: stats([row[ph] for p in pairs for row in p[r]]) for r in ('base', 'cand')}
            ratio = {s: st.median(100 * (stats([row[ph] for row in p['cand']])[s] /
                                         max(1e-9, stats([row[ph] for row in p['base']])[s]) - 1) for p in pairs)
                     for s in ('mean', 'median', 'p95', 'p99', 'max')}
            for r in ('base', 'cand'):
                s = pooled[r]
                print(f"  {ph:8} {r}: mean {s['mean']:.3f} median {s['median']:.3f} p95 {s['p95']:.3f} "
                      f"p99 {s['p99']:.3f} max {s['max']:.3f} ms" + (f", over 33.3 ms: {s['over']}" if ph == 'tick' else ''))
            print(f"  {ph:8} pair-median cand/base-1: " + ' '.join(f"{s} {ratio[s]:+.1f}%" for s in ratio))
        print()


if __name__ == '__main__':
    main()
