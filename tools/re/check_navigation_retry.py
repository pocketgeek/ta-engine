#!/usr/bin/env python3
"""Compare original navigator retry decisions with production World movement.

The controller's goal is not yet satisfied; active/inactive routes retain either
one or two points. Search
submission is observed at its boundary; no search worker/delivery runs. Native
RNG and complete 4e5150 retry branching execute unchanged. World uses its actual
requestPath queue, with scans deferred so they cannot replace input mode bits.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd,HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/cadence_test')
    args=ap.parse_args();p=Icd()
    game,unit,mover,kind,nav,controller,vt=[HEAP+i*0x30000 for i in range(7)]
    def put(fmt,address,*values): p.uc.mem_write(address,struct.pack('<'+fmt,*values))
    def get(address): return struct.unpack('<I',p.uc.mem_read(address,4))[0]
    put('I',0x62d55c,game);put('I',unit+8,mover);put('I',unit+0xb4,kind)
    put('I',nav+4,controller);put('I',nav+8,unit);put('I',controller,vt)
    player=HEAP+0x210000;put('I',unit+0xb8,player);put('B',player+0xeb,0)
    inactive=HEAP+0x200000;put('I',vt+0x10,inactive)
    p.hooks[inactive]=lambda uc,a:(1,0)
    requested=False
    def request(uc,address,size,data):
        nonlocal requested
        if get(uc.reg_read(UC_X86_REG_ESP)+4)!=1: raise AssertionError('unexpected request flag')
        requested=True
    p.uc.hook_add(UC_HOOK_CODE,request,begin=0x4e4f50,end=0x4e4f50)
    p.freeze_hooks()
    bounds=[]
    def rng_call(uc,address,size,data): bounds.append(get(uc.reg_read(UC_X86_REG_ESP)+4))
    p.uc.hook_add(UC_HOOK_CODE,rng_call,begin=0x535cc0,end=0x535cc0)
    put('3i',unit+0x68,128*65536,0,128*65536)
    put('4h',nav+0xc,128,128,800,800);put('I',nav+0x10c,2)
    rng=random.Random(0x4e5150);rows=[];expected=[]
    for i in range(8192):
        stamp=rng.randrange(0,1000);tick=stamp+rng.randrange(0,1800);seed=rng.getrandbits(32)
        outcome=i%16;mode=i//16%8;speed_mode=i//128%8
        streak=rng.randrange(3);boat=i//1024%2;scale=rng.choice([1,2,6,30,255])
        minimum_depth=rng.choice([-10000,-1,0,1,13,255])
        maximum_depth=rng.choice([0,20,255,10000])
        inactive=i//2048%2; count=1 if i//4096%2 else 2
        put('B',nav+0x114,0 if inactive else 1);put('I',nav+0x10c,count)
        put('I',0x634674,0)
        put('I',game+0x19f44,tick);put('I',nav+0x110,stamp);put('I',0x64186c,seed)
        put('B',unit+0x134,outcome)
        put('H',mover+0x36,1|(mode<<5)|(speed_mode<<8)|(4 if streak==2 else 8 if streak else 0))
        put('I',kind+0x260,0x80000 if boat else 0)
        put('2h',kind+0x192,maximum_depth,minimum_depth)
        put('B',kind+0x249,scale)
        requested=False;bounds.clear()
        _,error=p.call(0x4e5150,ecx=nav)
        if error: raise AssertionError((i,error))
        expected.append([int(requested),get(0x64186c),get(unit+0x134)&15,*bounds])
        rows.append(f'{tick} {stamp} {seed} {outcome} {mode} {speed_mode} {streak} {boat} {scale} {minimum_depth} {maximum_depth} {inactive} {count}')
    result=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    for i,(row,want,got) in enumerate(zip(rows,expected,actual)):
        if want!=got: raise AssertionError((i,row,'retail',want,'World',got))
    print(f'PASS: {len(rows)} native retry decisions, outcome flags, final RNG states and draw bounds against World')


if __name__=='__main__': main()
