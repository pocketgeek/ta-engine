#!/usr/bin/env python3
"""Observe full native sight-stamp admission with synthetic height-pair cells.

This tests 4c6800's consumers of the precomputed height-pair map; producing
that map from terrain is a separate requirement.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
game,player,revealer,counts,mapping,terrain=[HEAP+i*0x20000 for i in range(6)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
width,height=32,24
put(0x62d55c,game);put(game+0x19e98,width*2);put(game+0x19e9c,height*2)
put(game+0x19f0c,width);put(game+0x19f10,height);put(game+0x19f08,terrain)
put(game+0x19ef4,mapping);p.uc.mem_write(game+0x306f,b'\xff')
put(player+0x88,counts);put(player+0x8c,width);put(player+0x90,height)
p.uc.mem_write(player+0xeb,b'\x03')
rng=random.Random(0x4c69a2)
for case in range(512):
    cx,cz=rng.randrange(-4,width+4),rng.randrange(-4,height+4)
    sight,eye,eye_y=rng.randrange(1,257),rng.randrange(1,256),rng.randrange(-20,401)
    pairs=bytes(rng.randrange(256) for _ in range(width*height*2))
    p.uc.mem_write(terrain,pairs);p.uc.mem_write(counts,bytes(width*height))
    p.uc.mem_write(mapping,bytes(width*height*2));p.uc.mem_write(revealer,bytes(32));put(revealer,player)
    p.uc.mem_write(revealer+0x14,struct.pack('<hhihBB',cx,cz,eye_y,sight,eye,0))
    _,error=p.call(0x4c6800,(1,1),ecx=revealer);assert not error,error
    scale=struct.unpack('<f',struct.pack('<f',sight/(eye*32)))[0]
    radius=sight//16
    expected=[]
    for z in range(height):
        for x in range(width):
            dx,dz=x-cx,z-cz;distance=dx*dx+dz*dz
            inside=abs(dx)<=radius and abs(dz)<=radius
            index=z*width+x
            delta=eye_y-min(pairs[index*2:index*2+2])
            admitted=inside and (distance<=2 or (distance<=radius*radius and delta>=0 and distance<=(delta*scale)**2))
            expected.append(int(admitted))
    actual=bytes(p.uc.mem_read(counts,width*height))
    assert actual==bytes(expected),(case,cx,cz,sight,eye,eye_y,[(i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b][:8])
    words=struct.unpack('<'+'H'*(width*height),p.uc.mem_read(mapping,width*height*2))
    assert list(words)==[v*8 for v in expected]
print('PASS: 512 native sight stamps match near-center admission, bounded radius and height-pair scaled-distance tests')
