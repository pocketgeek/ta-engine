#!/usr/bin/env python3
"""Compare native land/boat scans, modes, deadlines, and ordered queries.

Placement grades and a uniform coarse visibility plane are controlled inputs.
Navigator points coincide, so corner pruning is checked by its separate oracle.
Original forward stepping, visibility reads, and neighbor scanning execute.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_MEM_READ


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args();p=Icd()
    game,unit,mover,kind,nav,table,world,owner,plane=[HEAP+i*0x30000 for i in range(9)]
    def put(fmt,a,*v): p.uc.mem_write(a,struct.pack('<'+fmt,*v))
    def get(fmt,a): return struct.unpack('<'+fmt,p.uc.mem_read(a,struct.calcsize('<'+fmt)))[0]
    put('I',0x62d55c,game);put('I',0x62d558,world);put('I',world,world+32)
    put('I',unit+8,mover);put('I',unit+0xb4,kind);put('I',unit+0xb8,owner)
    put('I',mover,nav);put('I',nav,table);put('I',table+0xc,HEAP+0x200000)
    put('I',game+0x19ef4,plane)
    def points(uc,a):
        assert get('I',a+4)==3
        uc.mem_write(get('I',a),bytes(36))
        return 2,0
    p.hooks[HEAP+0x200000]=points
    queries=[];probe=0;bad_index=0;bad_grade=0;base_grade=7;width=32
    def grade(uc,a):
        nonlocal probe
        who,x,y,z=struct.unpack('<Iiii',uc.mem_read(a,16));assert who==unit
        queries.extend((0,x,y,z))
        value=bad_grade if probe==bad_index else base_grade;probe+=1
        return 4,value
    p.hooks[0x4db640]=grade;p.freeze_hooks()
    def visible_read(uc,access,address,size,value,data):
        assert size==2
        index=(address-plane)//2;queries.extend((1,index%width,index//width))
    p.uc.hook_add(UC_HOOK_MEM_READ,visible_read,begin=plane,end=plane+65535)
    rng=random.Random(0x4dbb75);rows=[];expected=[]
    for i in range(10000):
        x,z=[rng.randrange(-64*65536,2048*65536) for _ in range(2)]
        y=rng.randrange(-256*65536,512*65536);heading=rng.randrange(65536)
        tick=rng.choice([0,100,0xfffffffe]);half=rng.choice([0,1,2,6,255])
        boat=i%4!=0;foot=rng.randrange(1,9)
        sight=rng.choice([-32768,-160,-1,0,16,159,160,161,176,256,512])
        disabled=i%37==0;width=rng.randrange(1,65);height=rng.randrange(1,65)
        player=rng.randrange(32);mask=rng.choice([0,65535,rng.randrange(65536)])
        bad_index=rng.choice([-1,0,1,7,8,9,10,11,12,16,24,40]);bad_grade=rng.randrange(-1,8)
        base_grade=rng.choice([5,6,7])
        # Ensure coverage at the 160px obstacle boundary with visible water.
        if i%5==0:
            boat=True;x=z=1024*65536;y=0;heading=0;sight=512
            width=height=64;player=0;mask=65535;bad_index=i//5%14;bad_grade=i//70%6-1
        put('3i',unit+0x68,x,y,z);put('H',unit+0x7e,heading);put('h',unit+0x78,foot)
        put('I',kind+0x260,0x80000 if boat else 0);put('h',kind+0x194,13 if boat else 0)
        put('h',kind+0x226,sight);put('B',kind+0x249,half)
        put('I',game+0x19f44,tick);put('B',game+0x306f,player)
        put('2I',owner+0x8c,width,height);put('B',world+32+0xa,int(disabled))
        put('H',mover+0x36,0xffff);p.uc.mem_write(plane,struct.pack('<H',mask)*4096)
        queries.clear();probe=0
        _,error=p.call(0x4dba80,(unit,),ecx=mover)
        if error: raise RuntimeError((i,error))
        flags=get('H',mover+0x36)
        expected.append([get('I',mover+0x30),(flags>>5)&7,(flags>>8)&7,*queries])
        rows.append(' '.join(map(str,[x,y,z,heading,tick,half,int(boat),foot,sight,
            int(disabled),width,height,player,mask,bad_index,bad_grade,base_grade])))
    result=subprocess.run([args.binary,'--ground-scan'],input='\n'.join(rows)+'\n',
        text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    for i,(row,want,got) in enumerate(zip(rows,expected,actual)):
        if want!=got: raise AssertionError((i,row,'retail',want,'port',got))
    print(f'PASS: {len(rows)} land/boat scans, modes, deadlines and ordered placement/visibility queries')


if __name__=='__main__': main()
