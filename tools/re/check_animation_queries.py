#!/usr/bin/env python3
"""Compare horizontal-speed and turn animation queries with the retail executable.

Unlike the earlier refusal-only GET 29 check, covers arbitrary vectors, individual
base speeds, terrain modifiers, percentage rounding and unsigned turn-rate words.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-o2/retail_visual_test')
    args=ap.parse_args()
    p=Icd();unit,mover,kind=[HEAP+i*0x10000 for i in range(3)]
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def word(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
    def signed(v):return v if v<0x80000000 else v-0x100000000
    put(unit+8,mover);put(unit+0xb4,kind)
    rng=random.Random(0x4dc100);rows=[];expected=[]
    for i in range(8192):
        dx,dz=[rng.randrange(-1000000,1000001) for _ in range(2)]
        base=rng.randrange(1,1000001)
        road,water=[rng.randrange(1,4*65536) for _ in range(2)]
        flags=rng.choice((0,4,8,0x800,0x1000,0x1800,0x1804))
        turn=rng.randrange(-32768,32768)
        moving,pivot=[rng.randrange(65536) for _ in range(2)]
        attached=int(i%17==0)
        if i%101==0:base=0
        if i%103==0:moving=pivot=0
        if i%107==0:road=water=0
        if i%109==0:turn=0
        put(unit+0xa8,attached);put(unit+0x12b,base)
        put(mover+8,dx);put(mover+16,dz);word(mover+0x36,flags);word(mover+0x24,turn)
        put(kind+0x172,road);put(kind+0x16e,water)
        word(kind+0x18e,moving);word(kind+0x190,pivot)
        results=[]
        for address in (0x4dc100,0x4dc3f0):
            value,error=p.call(address,(unit,),ecx=mover)
            if error:raise RuntimeError(error)
            results.append(signed(value))
        expected.append(tuple(results))
        rows.append(' '.join(map(str,(dx,dz,base,road,water,flags,turn,moving,pivot,attached))))
    result=subprocess.run([args.binary,'--animation-queries'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    if actual!=expected:
        raise AssertionError(next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch'))
    print(f'PASS: {len(rows)} native cases, {2*len(rows)} horizontal-speed and turn query results')


if __name__=='__main__':main()
