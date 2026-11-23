#!/usr/bin/env python3
"""Compare original navigator delivery (4e4ea0) with World::deliverSearchRoute.

Routes are independently authored search outputs. The goal acceptance virtual
and notification sink are controlled; delivery and pending-request removal run
their original code. This does not run the search or subsequent movement.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner');args=parser.parse_args()
    p=Icd();uc=p.uc
    nav,unit,mover,controller,table,player,points=(HEAP+i*0x1000 for i in range(7))
    def put(address,value): uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    def get(address): return struct.unpack('<I',uc.mem_read(address,4))[0]
    put(nav+4,controller);put(nav+8,unit);put(unit+8,mover);put(unit+0xb8,player)
    put(controller,table);put(table+0x10,HEAP+0x8000)
    uc.mem_write(player+0xeb,b'\x00')
    p.hooks[HEAP+0x8000]=lambda uc,a:(1,accepted)
    def notify(uc,a):
        events.append(get(a));return 1,0
    p.hooks[0x4e2470]=notify
    fixtures=[];expected=[];rng=random.Random(0x4e4ea0)
    for index in range(2048):
        fx,fz=rng.randrange(1,9),rng.randrange(1,9)
        accepted,inactive,flags=rng.randrange(2),rng.randrange(2),rng.randrange(16)
        count=rng.choice((0,2,3,63,64,65,128))
        route=[(rng.randrange(100),rng.randrange(100)) for _ in range(count)]
        # PathService exposes center cells; retail delivers integer world points.
        world=[(x*16+(fx%2)*8,z*16+(fz%2)*8) for x,z in route]
        uc.mem_write(points,b''.join(struct.pack('<hh',*point) for point in world))
        put(nav+0x10c,2);uc.mem_write(nav+0xc,struct.pack('<hhhh',128,128,256,384))
        uc.mem_write(nav+0x114,bytes([2|int(not inactive)]))
        put(nav+0x110,77);put(mover+0x30,123);put(unit+0x134,flags);put(0x634674,1)
        events=[]
        _,error=p.call(0x4e4ea0,(points,count),ecx=nav)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        navflags=uc.mem_read(nav+0x114,1)[0]
        assert not navflags&2 and get(0x634674)==0
        values=[navflags&1,sum(events),get(mover+0x30),get(nav+0x110),get(unit+0x134)&15]
        if count:
            n=get(nav+0x10c);values.extend((n,*struct.unpack('<'+'h'*(n*2),uc.mem_read(nav+0xc,n*4))))
        expected.append(' '.join(map(str,values)))
        fixtures.append(' '.join(map(str,(fx,fz,accepted,inactive,flags,count,*(v for point in route for v in point)))))
    actual=subprocess.run([args.runner,'--delivery'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} World route deliveries match retail activation, events, scan reset, flags and points')


if __name__=='__main__': main()
