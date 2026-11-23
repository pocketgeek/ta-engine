#!/usr/bin/env python3
"""Compare ground patrol stages/events against executable 403600.

Diversions and assistance are disabled; return-waypoint/controller construction
and combat selection are controlled hosts. Native stage and timer code executes.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_mission_test')
    args = parser.parse_args()
    p = Icd()
    unit, mission, kind, game = (HEAP+i*4096 for i in range(4))

    def put(address, *values):
        p.uc.mem_write(address, struct.pack('<'+'I'*len(values),
                                          *(v & 0xffffffff for v in values)))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    counters = {}

    def initialize(uc, address):
        counters['initialized'] += 1
        return 2, 0

    def reset(uc, address):
        counters['reset'] += 1
        counters['radius'] = get(address+4)
        return 2, 0

    def combat(uc, address):
        counters['actions'] += 1
        return 1, int(counters['respond'])

    def draw(uc, address, size, unused):
        counters['draws'] += 1

    p.hooks[0x51d1e0] = lambda uc,a: (3,0)
    p.hooks[0x4d6b80] = initialize
    p.hooks[0x519b10] = lambda uc,a: (1,0)
    p.hooks[0x4d4da0] = reset
    p.hooks[0x4d8370] = combat
    p.hooks[0x4d7de0] = lambda uc,a: (6,1)
    p.freeze_hooks()
    p.uc.hook_add(UC_HOOK_CODE, draw, begin=0x535cc0, end=0x535cc0)
    put(0x62d55c, game)
    put(unit+0xb4, kind)
    put(mission+0xe, unit)
    rng = random.Random(0x403600)
    fixtures, expected = [], []
    for i in range(5000):
        tick, seed = rng.randrange(2**32), rng.randrange(2**32)
        stage = i % 5
        radius = rng.choice((0, 4, 32, 100, 0xffffffff))
        mask, deadline, pending = (rng.randrange(2**32) for _ in range(3))
        flags = rng.choice((0, 0x08001411, 0x03001411))
        foot, mover, respond = rng.randint(1, 16), int(i%13 != 0), rng.randrange(2)
        events = rng.choice((0, 1, 0x100, 0x200, 0x400, 0x2000, 0x2701))
        counters.update(initialized=0, reset=0, radius=0, actions=0, draws=0, respond=respond)
        put(game+0x19f44, tick)
        put(0x64186c, seed)
        put(unit+8, mover)
        p.uc.mem_write(unit+0x78, struct.pack('<h', foot))
        p.uc.mem_write(mission+5, bytes((stage,)))
        put(mission+6, mask, deadline)
        put(mission+0x4e, radius)
        put(mission+0x5a, flags)
        put(mission+0x6a, pending)
        result, error = p.call(0x403600, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        expected.append((result, p.uc.mem_read(mission+5,1)[0], get(mission+0x4e),
                         get(mission+6),get(mission+10),get(mission+0x6a),get(mission+0x5a),
                         get(0x64186c),counters['initialized'],counters['reset'],
                         counters['radius'],counters['actions'],counters['draws']))
        fixtures.append(' '.join(map(str,(tick,seed,events,stage,radius,mask,deadline,pending,
                                         flags,foot,mover,respond))))
    output = subprocess.run([args.binary, '--ground-patrol'],
                            input='\n'.join(fixtures)+'\n', text=True,
                            capture_output=True, check=True)
    actual = [tuple(map(int,line.split())) for line in output.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('oracle row count mismatch')
    for i,(got,want) in enumerate(zip(actual,expected)):
        if got != want:
            raise AssertionError((i,fixtures[i],got,want))
    print(f'PASS: {len(fixtures)} ground patrol stage/event/timer cases, original executable')


if __name__ == '__main__':
    main()
