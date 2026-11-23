#!/usr/bin/env python3
"""Execute native damage-flame construction, authored clock and drawing.

Only final sprite submission is substituted; viewport and clock helpers run.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd()
particle,origin,animation,game=HEAP,HEAP+0x1000,HEAP+0x2000,HEAP+0x10000
draws=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def draw(uc,sp):
    draws.append(struct.unpack('<6i',uc.mem_read(sp,24)))
    return 6,0
p.hooks[0x4fac00]=draw
p.freeze_hooks()
put(0x62d55c,game)
for i,v in enumerate((-100000,-100000,100000,100000)):put(game+0x19e44+i*4,v)
rng=random.Random(0x4f2660)
steps=0
for case in range(1024):
    position=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    p.uc.mem_write(origin,struct.pack('<3i',*position))
    count=rng.randrange(1,18);loop=case%256
    durations=[rng.randrange(0,8) for _ in range(count)]
    p.uc.mem_write(animation,struct.pack('<HB',count,loop))
    for i,duration in enumerate(durations):
        put(animation+0x28+i*8,1234+i)
        p.uc.mem_write(animation+0x2c+i*8,struct.pack('<H',duration))
    _,error=p.call(0x4f2660,(origin,animation),ecx=particle)
    assert not error,error
    frame=0;remaining=durations[0];active=True
    for age in range(15):
        if age:
            if remaining>1:remaining-=1
            else:
                frame+=1
                if frame==count:
                    if loop:frame=0
                    else:active=False;frame=count-1
                if active:remaining=durations[frame]
        if age:
            value,error=p.call(0x4f25d0,(),ecx=particle)
            assert not error,error
            assert bool(value&255)==active,(case,age,value,active)
            if not active:break
        assert bytes(p.uc.mem_read(particle,12))==bytes(p.uc.mem_read(origin,12))
        draws.clear()
        _,error=p.call(0x4f2600,(),ecx=particle)
        assert not error,error
        x,y,z=[v>>16 for v in position]
        assert draws==[(1234+frame,x,z-(y>>1),1,0,0)],(case,age,draws,frame)
        steps+=1
    else:
        # Hard lifetime wins before the animation clock can advance on tick 15.
        before=bytes(p.uc.mem_read(particle+12,12))
        value,error=p.call(0x4f25d0,(),ecx=particle)
        assert not error and not value&255
        assert bytes(p.uc.mem_read(particle+12,12))==before
print(f'PASS: {steps} native damage-flame draws across 1024 lifetimes retain emission position, authored timing, draw flags and 15-tick cap')
