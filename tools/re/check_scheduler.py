#!/usr/bin/env python3
"""Compare C++ admission/accounting against executable 0x416430.

Both sides use the same controlled phase workloads. Does not certify path
geometry, actual phase work, entity allocation or whole-game parity.
"""
import argparse
import random
import subprocess
import struct

from emuscheduler import Scheduler, GS
from emu import Icd, HEAP


def check_admission(binary):
    p=Icd()
    game,nav=HEAP,HEAP+0x20000
    put=lambda address,value: p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    put(0x62d55c,game)
    rng=random.Random(0x4e54a0)
    cases=[(pending,tick,stamp) for pending in (0,1)
           for stamp in (0,1,10071,0xfffffff0,0xfffffff1,0xffffffff)
           for tick in (0,14,15,16,(stamp+14)&0xffffffff,(stamp+15)&0xffffffff)]
    cases += [(rng.randrange(2),rng.randrange(2**32),rng.randrange(2**32)) for _ in range(4000)]
    expected=[]
    for pending,tick,stamp in cases:
        put(game+0x19f44,tick); put(nav+0x110,stamp); put(nav+0x114,pending*2)
        result,error=p.call(0x4e54a0,ecx=nav)
        if error: raise RuntimeError(error)
        expected.append([int(result!=0),struct.unpack('<I',p.uc.mem_read(nav+0x110,4))[0]])
    proc=subprocess.run([str(binary),'--admission'],
                        input=''.join(f'{pending} {tick} {stamp}\n' for pending,tick,stamp in cases),
                        text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        index=next(i for i,pair in enumerate(zip(expected,actual)) if pair[0]!=pair[1])
        raise AssertionError((cases[index],expected[index],actual[index]))
    print(f'PASS: {len(cases)} executable navigator admission gates and timestamps')


def check(binary, requests, slots, budget, divisor, ticks, priorities):
    retail = Scheduler(requests, slots, budget, priorities)
    expected = []
    for tick in range(1, ticks + 1):
        retail.write(GS + 0x19f44, tick)
        state = retail.step(divisor)
        active = state['active'] or (-1, -1)
        values = [*active, state['phase'], state['remaining'], retail.read(0x114) & 255]
        values += state['players']
        values += [(retail.read(0x115 + p*4) - retail.entity(p, 0)) // 0x138 for p in range(10)]
        values += [retail.read(0x169 + p*4) for p in range(10)]
        expected.append('S ' + ' '.join(map(str, values)))
    kinds = {'init': 0, 'contour': 1, 'done': 2}
    expected += [f'E {tick} {kinds[event]} {key[0]} {key[1]}' for tick, event, key in retail.events]
    expected.append('END')
    mask = sum(1 << p for p in priorities)
    fixture = f'{slots} {budget} {divisor} {ticks} {mask} {len(requests)}\n'
    fixture += ''.join(f'{p} {s} {pops}\n' for (p, s), pops in requests.items())
    actual = subprocess.run([str(binary), '--oracle'], input=fixture, text=True,
                            capture_output=True, check=True).stdout.splitlines()
    if expected != actual:
        first = next((i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1]),
                     min(len(expected), len(actual)))
        raise AssertionError({'fixture': fixture, 'line': first,
                              'retail': expected[first:first+1], 'port': actual[first:first+1]})
    return ticks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build-dbg/retail_scheduler_test')
    args = parser.parse_args()
    check_admission(args.binary)
    fixtures = [({(0, 0): 1, (0, 2): 1, (1, 1): 1}, 8, 12000, 1, 3, ()),
                ({(0, 1): 1, (1, 1): 2000}, 8, 12000, 1, 4, ()),
                ({(0, 1): 1}, 8, 100, 1, 8, ()),
                ({(0, 1): 1}, 8, 7, 1, 12, ()),
                ({(9, 0): 1}, 1, 12000, 1, 3, (9,)),
                ({}, 8, 12000, 1, 3, ())]
    rng = random.Random(0x416430)
    for _ in range(100):
        slots = rng.choice((1, 2, 8, 16, 32))
        players = rng.sample(range(10), rng.randint(1, 10))
        requests = {(p, s): rng.choice((1, 5, 37, 150, 2000))
                    for p in players for s in rng.sample(range(slots), rng.randint(1, slots))}
        fixtures.append((requests, slots, rng.choice((7, 100, 503, 1000, 12000)),
                         rng.choice((1, 2, 3, 7)), 12,
                         tuple(p for p in players if rng.randrange(2))))
    ticks = sum(check(args.binary, *fixture) for fixture in fixtures)
    print(f'PASS: {len(fixtures)} executable scheduler fixtures, {ticks} tick boundaries and all phase events')


if __name__ == '__main__':
    main()
