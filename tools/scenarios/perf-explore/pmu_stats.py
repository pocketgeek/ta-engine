#!/usr/bin/env python3
"""pmu_stats.py FILE... [--warm N] [--phase explore] -- work counters of TAK_PMU runs.

Each FILE is the stderr of one legion_scenario (or takclient) run with TAK_PHASE=1 TAK_PHASE_MS=0 TAK_PMU=1 on ONE
core. Prints, per file, the median per tick (warm-up ticks dropped, default 100) of the phase's SIMPMU counters --
user-mode cycles, instructions, cycles stalled on L1-miss loads, LLC misses -- and the mean ms of that phase and of
the whole tick from the SIMPHASE lines. Counter runs are for work, not for milliseconds (TAK_PMU reads counters at
every phase boundary).
"""
import statistics as st, sys


def main():
    args = sys.argv[1:]
    warm, phase = 100, 'explore'
    if '--warm' in args:
        i = args.index('--warm'); warm = int(args[i + 1]); del args[i:i + 2]
    if '--phase' in args:
        i = args.index('--phase'); phase = args[i + 1]; del args[i:i + 2]
    for path in args:
        pmu, ms, tick = [], [], []
        for line in open(path):
            if line.startswith('SIMPMU'):
                d = dict(kv.split('=', 1) for kv in line.split()[1:])
                pmu.append([int(x) for x in d[phase].split('/')])
            elif line.startswith('SIMPHASE'):
                d = dict(kv.split('=') for kv in line.split()[1:])
                ms.append(float(d[phase])); tick.append(float(d['tick'].rstrip('ms')))
        pmu, ms, tick = pmu[warm:], ms[warm:], tick[warm:]
        if not pmu:
            print(f"{path}: no SIMPMU lines after {warm} warm-up ticks"); continue
        names = ['cycles', 'instructions', 'l1miss_stall_cycles', 'llc_misses']
        print(f"{path}: {len(pmu)} ticks, {phase} median per tick " +
              ' '.join(f"{n}={st.median(r[i] for r in pmu):.0f}" for i, n in enumerate(names)) +
              f"; mean ms {phase}={st.mean(ms):.3f} tick={st.mean(tick):.3f}")


if __name__ == '__main__':
    main()
