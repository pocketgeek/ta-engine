#!/usr/bin/env python3
"""Observe feature world origin before burn-smoke jitter in 4931e0.

Only terrain height lookup is substituted; the native signed footprint/cell
conversion and output fixed-point packing execute unchanged.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd();out,cell,definition=[HEAP+i*0x10000 for i in range(3)]
height=0;samples=[]
def terrain(uc,sp):
    point=struct.unpack('<I',uc.mem_read(sp,4))[0]
    x=struct.unpack('<I',uc.mem_read(point,4))[0]
    z=struct.unpack('<I',uc.mem_read(point+8,4))[0]
    samples.append((x,z));return 1,height & 0xffffffff
p.hooks[0x511170]=terrain;p.freeze_hooks()
rng=random.Random(0x4931e0)
for case in range(2048):
    cx,cz,fx,fz=[rng.randrange(-32768,32768) for _ in range(4)]
    height=rng.randrange(-65536,65536)
    p.uc.mem_write(cell,struct.pack('<hh',cx,cz))
    p.uc.mem_write(definition+0xb0,struct.pack('<hh',fx,fz))
    samples.clear();result,error=p.call(0x4931e0,(out,cell,definition))
    assert not error,error
    x=((2*cx+fx)<<19)&0xffffffff;z=((2*cz+fz)<<19)&0xffffffff
    actual=struct.unpack('<3I',p.uc.mem_read(out,12))
    assert result==out and actual==(x,(height<<16)&0xffffffff,z),(case,actual)
    assert samples==[(x,z)],(case,samples)
print('PASS: 2048 native feature origins use footprint center in 16.16 and terrain height at that center before smoke jitter')
