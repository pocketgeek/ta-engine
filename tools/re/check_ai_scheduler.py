#!/usr/bin/env python3
"""Compare AI dispatch with the original; membership/planner callbacks are controlled."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_ai_test')
    args=ap.parse_args()
    p=Icd(); game,manager,groups,table=HEAP,HEAP+0x30000,HEAP+0x40000,HEAP+0x50000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game); put(table,HEAP+0x60000)
    events=[]; tick=0; mutate=False
    def refresh(uc,a):
        events.append(-1)
        if mutate:
            put(manager+0x11+98*4,groups+98*32);put(groups+98*32+12,tick)
        return 0,0
    def assign(uc,a):
        events.append(-2)
        if mutate:put(manager+0x11,groups);put(groups+12,tick)
        return 0,0
    def update(uc,a):
        address=uc.reg_read(UC_X86_REG_ECX)
        events.append((address-groups)//32);put(address+12,tick+17)
        return 0,0
    p.hooks[0x40fe80]=refresh;p.hooks[0x40eeb0]=assign;p.hooks[HEAP+0x60000]=update
    p.freeze_hooks()
    rng=random.Random(0x40fd80);rows=[];expected=[]
    for case in range(4096):
        tick=rng.choice([0,1,10084,0xffffffff,rng.getrandbits(32)])
        seed=rng.getrandbits(32); mutate=case%3==0
        countdown=rng.choice([-2147483648,-1,0,1,2,30,2147483647])
        p.uc.mem_write(manager+0x11,bytes(400));events.clear()
        put(manager+5,countdown);put(game+0x19f44,tick);put(0x64186c,seed)
        initial=[]
        for i in range(100):
            address=groups+i*32;put(address,table);put(address+12,0)
            if rng.randrange(5):continue
            deadline=rng.choice([tick,(tick-1)&0xffffffff,(tick+1)&0xffffffff,0,0xffffffff])
            put(manager+0x11+i*4,address);put(address+12,deadline)
            initial.extend([i,deadline])
        _,error=p.call(0x40fd80,ecx=manager)
        if error:raise RuntimeError(error)
        after=struct.unpack('<i',struct.pack('<I',get(manager+5)))[0]
        row=[after,get(0x64186c),len(events),*events]
        for i in range(100):
            pointer=get(manager+0x11+i*4)
            row.extend([int(bool(pointer)),get(pointer+12) if pointer else 0])
        expected.append(row)
        rows.append(' '.join(map(str,[countdown,tick,seed,len(initial)//2,int(mutate),*initial])))
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} AI schedules, callback ordering, mutations and signed/unsigned wrap boundaries')


if __name__=='__main__':main()
