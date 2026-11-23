#!/usr/bin/env python3
"""Compare patrol steering-point construction and RNG with the retail handler.

Diversions are disabled and controller allocation/installation are host stubs.
The original stage-one mathematics and random calls execute unmodified.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=parser.parse_args()
    p=Icd()
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    unit,mission,kind,game,controller=[HEAP+i*4096 for i in range(5)]
    def put(address,*values):
        p.uc.mem_write(address,struct.pack('<'+'I'*len(values),*(v&0xffffffff for v in values)))
    def get(address): return struct.unpack('<I',p.uc.mem_read(address,4))[0]
    put(0x62d55c,game); put(unit+0xb4,kind); put(unit+0xa4,1)
    p.hooks[0x51d1e0]=lambda uc,a:(3,0)
    p.hooks[0x4eb9e0]=lambda uc,a:(0,controller)
    def construct(uc,a):
        p.uc.mem_write(controller+0x26,bytes(uc.mem_read(get(a+4),12)))
        return 2,controller
    def radius(uc,a):
        put(controller+0xa,get(a)); return 1,0
    p.hooks[0x4e40e0]=construct
    p.hooks[0x4e4540]=radius
    p.hooks[0x4d4d40]=lambda uc,a:(1,0)
    p.freeze_hooks()
    rng=random.Random(0x41a47e)
    fixtures,expected=[],[]
    for i in range(4000):
        position=tuple(rng.randrange(10000*65536) for _ in range(3))
        extent=(16,64,320,1024,10000)[i%5]*65536
        target=tuple(v+rng.randrange(-extent,extent+1) for v in position)
        maximum=rng.choice((3*65536,3*65536+1,5*65536))
        seed=rng.randrange(2**32)
        put(unit+0x68,*position); put(mission+0x22,*target); put(kind+0x162,maximum)
        put(0x64186c,seed); put(mission+0x5a,0)
        p.uc.mem_write(mission+5,b'\x01')
        result,error=p.call(0x419db0,(unit,mission,0))
        if error or result!=1: raise AssertionError((result,error))
        point=struct.unpack('<3i',p.uc.mem_read(controller+0x26,12))
        expected.append((*point,get(controller+0xa),get(0x64186c)))
        fixtures.append('j '+' '.join(map(str,(*position,*target,maximum,seed))))
    output=subprocess.run([args.binary,'--oracle'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in output.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError('oracle row count mismatch')
    for i,(got,want) in enumerate(zip(actual,expected)):
        if got!=want: raise AssertionError((i,fixtures[i],got,want))
    print(f'PASS: {len(fixtures)} patrol points, acceptance radii and final RNG states')


if __name__=='__main__': main()
