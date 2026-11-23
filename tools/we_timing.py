#!/usr/bin/env python3
"""we_timing.py OUTDIR [--warm N] -- summarise a tools/we_timing.sh round.

OUTDIR/runs/r<round>-<situation>-<base|cand>-<01|23> hold the SIMPHASE lines of one run (and SIMSTATS with --counters).
Per situation and phase: the median over runs of each build's mean ms per tick (warm-up ticks dropped), the ratio of
those medians, and the median/range of the per-pair ratios (base and cand ran concurrently on sibling cores, which
cancels most of the host noise). The pair ratio is the headline number: cand/base - 1, negative = cand faster.
"""
import glob, os, re, statistics as st, sys

PHASES = ['tick', 'combat', 'sep', 'grid', 'explore', 'scripts', 'movement', 'nav', 'prologue', 'other']


def load(path, warm):
    ph, ss = [], []
    for line in open(path):
        if line.startswith('SIMPHASE'):
            d = dict(kv.split('=') for kv in line.split()[1:])
            ph.append({k: float(v.rstrip('ms')) for k, v in d.items()})
        elif line.startswith('SIMSTATS'):
            ss.append({k: float(v) for k, v in (kv.split('=') for kv in line.split()[1:])})
    return ph[warm:], ss[warm:]


def mean(rows, k):
    return st.mean(r[k] for r in rows) if rows else float('nan')


def main():
    out = sys.argv[1]
    warm = int(sys.argv[sys.argv.index('--warm') + 1]) if '--warm' in sys.argv else 1500
    runs = {}   # (sit, round, pair) -> {role: summary}
    wall = {}
    for f in sorted(glob.glob(os.path.join(out, 'runs', 'r*-*-*-??'))):
        m = re.match(r'r(\d+)-(.+)-(base|cand)-(01|23)$', os.path.basename(f))
        if not m:
            continue
        rnd, sit, role, cores = m.groups()
        ph, ss = load(f, warm)
        if not ph:
            continue
        w = [l.split()[1] for l in open(f + '.out') if l.startswith('wall')] if os.path.exists(f + '.out') else []
        hs = [l.split()[-1] for l in open(f + '.out') if 'hash' in l] if os.path.exists(f + '.out') else []
        rec = {'n': len(ph), 'u0': ph[0]['units'], 'u1': ph[-1]['units'], 'hash': hs[0] if hs else '',
               'ms': {k: mean(ph, k) for k in PHASES}, 'stats': {k: mean(ss, k) for k in ss[0]} if ss else {}}
        runs.setdefault((sit, rnd, cores), {})[role] = rec
        wall[f] = float(w[0]) if w else 0.0
    sits = sorted({k[0] for k in runs})
    for sit in sits:
        pairs = [v for k, v in sorted(runs.items()) if k[0] == sit and 'base' in v and 'cand' in v]
        if not pairs:
            continue
        b0 = pairs[0]['base']
        print(f"## {sit}: {len(pairs)} concurrent pairs, {b0['n']} ticks measured after {warm} warm-up, "
              f"units {b0['u0']:.0f} -> {b0['u1']:.0f}")
        print(f"{'phase':9} {'base ms':>9} {'cand ms':>9} {'ratio %':>8} {'pair median %':>14} {'pair range %':>16}")
        for k in PHASES:
            bs = [p['base']['ms'][k] for p in pairs]
            cs = [p['cand']['ms'][k] for p in pairs]
            pr = [100 * (p['cand']['ms'][k] / p['base']['ms'][k] - 1) for p in pairs if p['base']['ms'][k] > 0]
            if not pr:
                continue
            bm, cm = st.median(bs), st.median(cs)
            if bm <= 0 or cm <= 0:   # a phase the mode never runs (nav under Retail)
                continue
            print(f"{k:9} {bm:9.3f} {cm:9.3f} {100 * (cm / bm - 1):+8.1f} {st.median(pr):+14.1f} "
                  f"{min(pr):+8.1f}..{max(pr):+.1f}")
        hb = {p['base']['hash'] for p in pairs}
        hc = {p['cand']['hash'] for p in pairs}
        print(f"final state hash: base {','.join(sorted(hb))} cand {','.join(sorted(hc))}"
              f" ({'identical' if hb == hc else 'DIFFERENT'})")
        if pairs[0]['base']['stats']:
            ks = [k for k in pairs[0]['base']['stats'] if k not in ('tick', 'units')]
            print('counters (per tick, base -> cand): ' + ' '.join(
                f"{k}={st.median(p['base']['stats'][k] for p in pairs):.0f}->{st.median(p['cand']['stats'][k] for p in pairs):.0f}"
                for k in ks))
        print()
    if wall:
        print(f"summed run wall {sum(wall.values()) / 2:.0f} s of pair time")


main()
