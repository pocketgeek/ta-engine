#!/usr/bin/env python3
"""Observe native clicked-point height on controlled flat terrain.

Executes the complete 510da0 picker, replacing only its terrain-height query.
This establishes flat terrain/water height, not all sloped picking geometry.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd()
game,point=HEAP,HEAP+0x20000
height=0
queries=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def terrain(uc,sp):
    address=struct.unpack('<I',uc.mem_read(sp,4))[0]
    queries.append(struct.unpack('<3i',uc.mem_read(address,12)))
    return 1,height & 0xffffffff
p.hooks[0x511170]=terrain
p.freeze_hooks()
put(0x62d55c,game);put(game+0x19e88,2048);put(game+0x19e8c,2048)
rng=random.Random(0x510da0)
for case in range(2048):
    height=rng.randrange(-1,256);sea=rng.randrange(256)
    p.uc.mem_write(game+0x19ef8,bytes([sea]))
    x,z=rng.randrange(-128,2176),rng.randrange(128,1800)
    queries.clear()
    _,error=p.call(0x510da0,(x,z,point));assert not error,error
    actual=struct.unpack('<3i',p.uc.mem_read(point,12))
    assert actual[0]==min(2047,max(0,x))*65536,(case,actual,x)
    assert actual[1]==max(height,sea)*65536,(case,actual,height,sea)
    assert queries
print('PASS: 2048 native clicked destinations clamp X and use max(terrain height, sea level) on controlled flat terrain')
