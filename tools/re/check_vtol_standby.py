#!/usr/bin/env python3
"""Check ordinary VTOL_Standby transitions with controlled targeting/landing.

Boundary recovery and diversion are held inactive; they are separate handlers.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_mission_test')
    args=ap.parse_args(); p=Icd()
    unit,mission,game,mover,kind,landing,scratch=[HEAP+n*0x30000 for n in range(7)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);put(unit+0xa4,1);put(unit+0xb4,kind);put(mission+0xe,unit)
    p.hooks[0x51d1e0]=lambda uc,a:(3,0)
    p.hooks[0x4d7de0]=lambda uc,a:(6,1)
    p.hooks[0x4d4bf0]=lambda uc,a:(12,scratch)
    p.hooks[0x4d6c40]=lambda uc,a:(1,landing)
    p.hooks[0x4d7750]=lambda uc,a:(2,0)
    p.hooks[0x4d6da0]=lambda uc,a:(0,0)
    p.hooks[0x4eba00]=lambda uc,a:(0,0)
    rng=random.Random(0x417350);rows=[];expected=[]
    for i in range(1024):
        tick=rng.choice([0,10070,0xfffffffc,rng.randrange(2**32)])
        seed,mask,deadline,pending,flags=[rng.randrange(2**32) for _ in range(5)]
        stage=i%4;mover_on=(i//4)%2;fly=(i//8)%2;air=(i//16)%2;install=(i//32)%2;land=(i//64)%2
        put(unit+8,mover if mover_on else 0);put(unit+0x130,2 if air else 1);put(kind+0x260,0x800 if fly else 0)
        put(game+0x19f44,tick);put(0x64186c,seed)
        p.uc.mem_write(mission+5,bytes([stage]));p.uc.mem_write(landing+4,bytes([land]))
        for off,v in [(6,mask),(10,deadline),(0x6a,pending),(0x5a,flags)]:put(mission+off,v)
        calls=[0,0,0]
        def initialize(uc,a):calls[0]+=1;return 1,0
        def choose(uc,a):calls[1]+=1;return 1,install
        def allocate(uc,a):calls[2]+=1;return 0,landing
        p.hooks[0x519b70]=initialize;p.hooks[0x4d8370]=choose;p.hooks[0x4eb9e0]=allocate
        result,error=p.call(0x417350,(unit,mission,0))
        if error:raise RuntimeError(error)
        rows.append(' '.join(map(str,[tick,seed,stage,mask,deadline,pending,flags,mover_on,fly,air,install,land])))
        expected.append([result,p.uc.mem_read(mission+5,1)[0],get(mission+6),get(mission+10),get(mission+0x6a),get(mission+0x5a),get(0x64186c),*calls])
    proc=subprocess.run([args.binary,'--vtol-standby'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if len(actual)!=len(expected):raise AssertionError('row count')
    for row,want,got in zip(rows,expected,actual):
        if want!=got:raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} original VTOL_Standby transitions, callbacks and RNG boundaries')


if __name__=='__main__':main()
