#!/usr/bin/env python3
"""phase_stats.py WE_OUT [--warm N] -- per-tick distributions of a tools/we_timing.sh round.

WE_OUT/runs/r<round>-<sit>-<base|cand>-<01|23> hold the SIMPHASE lines of one run. For each situation, build and
phase (tick= and explore=), every measured tick of every run is pooled (warm-up ticks dropped) and its mean,
median, p95, p99 (nearest rank), max and the ticks over 33.3 ms printed; then the median and range over the
concurrent pairs of cand/base - 1 of the run means (negative = candidate faster), the number that cancels most
host noise. SIMPHASE prints 0.01 ms steps, so phases below about 0.05 ms are only roughly resolved.
"""
import glob, math, os, re, statistics as st, sys

PHASES = ['tick', 'explore']


def load(path, warm):
    rows = []
    for line in open(path):
        if line.startswith('SIMPHASE'):
            d = dict(kv.split('=') for kv in line.split()[1:])
            rows.append({k: float(v.rstrip('ms')) for k, v in d.items()})
    return rows[warm:]


def rank(xs, q):
    xs = sorted(xs)
    return xs[max(0, math.ceil(q * len(xs)) - 1)]


def main():
    out = sys.argv[1]
    warm = int(sys.argv[sys.argv.index('--warm') + 1]) if '--warm' in sys.argv else 100
    runs = {}
    for f in sorted(glob.glob(os.path.join(out, 'runs', 'r*-*-*-??'))):
        m = re.match(r'r(\d+)-(.+)-(base|cand)-(01|23)$', os.path.basename(f))
        if not m:
            continue
        rnd, sit, role, cores = m.groups()
        rows = load(f, warm)
        if rows:
            runs.setdefault(sit, {}).setdefault((rnd, cores), {})[role] = rows
    for sit in sorted(runs):
        pairs = [v for _, v in sorted(runs[sit].items()) if 'base' in v and 'cand' in v]
        if not pairs:
            continue
        n = len(pairs[0]['base'])
        units = pairs[0]['base'][0]['units'], pairs[0]['base'][-1]['units']
        print(f"## {sit}: {len(pairs)} concurrent pairs x {n} ticks after {warm} warm-up, units {units[0]:.0f} -> {units[1]:.0f}")
        print(f"{'phase':8} {'build':5} {'mean':>8} {'median':>8} {'p95':>8} {'p99':>8} {'max':>8} {'>33.3':>6}"
              f"   (ms per tick; >33.3: ticks over a 30 Hz frame)")
        for k in PHASES:
            for role in ('base', 'cand'):
                xs = [r[k] for p in pairs for r in p[role]]
                print(f"{k:8} {role:5} {st.mean(xs):8.4f} {st.median(xs):8.4f} {rank(xs, .95):8.4f} "
                      f"{rank(xs, .99):8.4f} {max(xs):8.4f} {sum(x > 1000 / 30 for x in xs):6d}")
            pr = [100 * (st.mean(r[k] for r in p['cand']) / st.mean(r[k] for r in p['base']) - 1)
                  for p in pairs if st.mean(r[k] for r in p['base']) > 0]
            if pr:
                print(f"{k:8} pairs cand/base-1: median {st.median(pr):+.1f}%  range {min(pr):+.1f}..{max(pr):+.1f}%")
        print()


if __name__ == '__main__':
    main()
