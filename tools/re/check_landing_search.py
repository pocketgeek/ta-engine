#!/usr/bin/env python3
"""Compare VTOL landing candidate draws, footprint snapping and fallback orbit.

Uses the full original mission with boundary/diversion disabled and a controlled
landing predicate. Actual terrain feasibility and landing descent are separate.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args();p=Icd()
    unit,mission,game,controller=[HEAP+n*0x30000 for n in range(4)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);put(unit+0xa4,1);p.uc.mem_write(mission+5,b'\x02')
    p.hooks[0x51d1e0]=lambda uc,a:(3,0)
    p.hooks[0x4eb9e0]=lambda uc,a:(0,controller)
    p.hooks[0x4d4d40]=lambda uc,a:(1,0)
    rng=random.Random(0x417188);rows=[];expected=[]
    for i in range(1024):
        position=[rng.randrange(-1000000000,1000000000) for _ in range(3)]
        anchor=[rng.randrange(-1000000000,1000000000) for _ in range(3)]
        fx,fz=rng.randrange(1,16),rng.randrange(1,16)
        events=rng.choice([0,0x100,0x400,0x500])
        seed,angle=rng.randrange(2**32),rng.randrange(2**32);accept=i%13
        p.uc.mem_write(unit+0x68,struct.pack('<3i',*position));p.uc.mem_write(unit+0x78,struct.pack('<2h',fx,fz))
        p.uc.mem_write(mission+0x22,struct.pack('<3i',*anchor));put(mission+0x4e,angle);put(0x64186c,seed)
        probes=[-1];result_point=[];flags=[0]
        def landable(uc,a):
            probes[0]+=1
            return 2,int(probes[0]>0 and probes[0]==accept)
        def create(uc,a):
            result_point[:]=struct.unpack('<3i',uc.mem_read(get(a+4),12));return 2,controller
        def flag(uc,a):flags[0]=get(a);return 1,0
        p.hooks[0x509400]=landable;p.hooks[0x4e40e0]=create;p.hooks[0x4e4540]=flag
        result,error=p.call(0x416cd0,(unit,mission,events))
        if error or result!=2:raise RuntimeError((error,result))
        rows.append('l '+' '.join(map(str,[*position,*anchor,fx,fz,events,angle,seed,accept])))
        expected.append([*result_point,get(mission+0x4e),get(0x64186c),int(flags[0]==0x40),probes[0]])
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if len(actual)!=len(expected):raise AssertionError('row count')
    for row,want,got in zip(rows,expected,actual):
        if want!=got:raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} original landing searches, RNG states, snapped candidates and orbit fallback')


if __name__=='__main__':main()
