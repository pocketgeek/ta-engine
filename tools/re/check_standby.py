#!/usr/bin/env python3
"""Compare Standby state transitions with retail using controlled host callbacks.

Diversion/target selection are explicit host inputs, not certified by this test.
The actual 407770 handler and 4d6a10 timer execute under Unicorn.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build-dbg/retail_mission_test')
    args = parser.parse_args()
    p = Icd()
    unit, mission, game, mover = HEAP, HEAP+0x200, HEAP+0x1000, HEAP+0x30000
    def put(address, value): p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    def get(address): return struct.unpack('<I', p.uc.mem_read(address, 4))[0]
    put(0x62d55c, game)
    p.hooks[0x51d1e0] = lambda uc,a:(3,0)
    p.hooks[0x519b10] = lambda uc,a:(1,0)
    rng = random.Random(0x407770)
    fixtures, expected = [], []
    for i in range(512):
        tick = rng.choice((0, 10070, 0xfffffffc, rng.randrange(2**32)))
        seed, stage, mask = rng.randrange(2**32), i%4, rng.randrange(2**32)
        deadline, pending, flags = (rng.randrange(2**32) for _ in range(3))
        has_mover, install = (i//4)%2, (i//8)%2
        put(unit+8, mover if has_mover else 0)
        p.uc.mem_write(mission+5, bytes((stage,)))
        for offset, value in ((6,mask),(10,deadline),(0x6a,pending),(0x5a,flags)):
            put(mission+offset,value)
        put(game+0x19f44,tick); put(0x64186c,seed)
        calls = [0,0]
        def initialize(uc,a): calls[0] += 1; return 0,0
        def choose(uc,a): calls[1] += 1; return 1,1 if install else 0
        p.hooks[0x4dc0e0] = initialize
        p.hooks[0x4d8370] = choose
        p.hooks[0x4d7de0] = lambda uc,a:(6,1)
        result,error=p.call(0x407770,(unit,mission,0))
        assert error is None,error
        fixtures.append(' '.join(map(str,(tick,seed,stage,mask,deadline,pending,flags,has_mover,install))))
        expected.append(' '.join(map(str,(result,p.uc.mem_read(mission+5,1)[0],get(mission+6),
            get(mission+10),get(mission+0x6a),get(mission+0x5a),get(0x64186c),*calls))))
    actual=subprocess.run([args.binary,'--standby'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)
    print(f'PASS: {len(fixtures)} Standby handler transitions, timers, RNG and host calls match retail')


if __name__=='__main__': main()
