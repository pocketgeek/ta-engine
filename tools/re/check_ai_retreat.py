#!/usr/bin/env python3
"""Compare the Classic brain's Monarch retreat with retail's.

Runs retail's retreat 0x40d010(unit) for one mobile member of a base squad on
the cases of check_ai_defence.py (the same base centre 0x40c610, ring-0 radius
0x40cf30 with its 320 fallback, enemy query 0x411ae0, trig, _ftol and 64-bit
multiplies running as retail's own code; the placement test 0x507d10, the
order-kind lookup 0x4de530 and the order 0x4d78a0 logged and answered from the
case). The cases cover flyers (no placement test, no bounds test, the queued
patrol leg), every point refused, no enemy near (the first kept point), the
farthest-from-its-nearest-enemy choice with the score starting at 0, and kinds
that do not resolve.

The port (classic_ai_test --retreat: ai::classicRetreat with the brain's squad
helpers and ai::classicEnemiesNear) must make the same draws, placement
queries, kind lookups and orders in the same order, and leave the same seed.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_retreat.py --binary build-o2/classic_ai_test
"""
import argparse

from check_ai_defence import run


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    args = ap.parse_args()
    n, stats = run('retreat', args.binary, args.cases)
    print('PASS: %d 0x40d010 Monarch retreat calls, 0 mismatches: %d orders, %d with a patrol leg looked up, '
          '%d issuing nothing, %d flyers; retail paths reached: %s'
          % (n, stats['orders'], stats['patrols'], stats['nothing'], stats['flyers'], stats['paths']))


if __name__ == '__main__':
    main()
