#!/usr/bin/env python3
"""Observe native visibility-height map construction from terrain cell heights.

Executes the accumulation phase 50ea58..50ecc8, with only allocation, free
and the phase boundary controlled. This stops before weighted smoothing and
the sea-height clamp; check_exploration.py verifies the complete producer
through 50ed53 against the production helper. Earlier map loading is excluded.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_EBP
p=Icd()
game,cells,out,frame=[HEAP+i*0x40000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
p.hooks.update({0x4eba00:lambda uc,sp:(0,0),0x4eb9e0:lambda uc,sp:(0,out),
                0x50ecc8:lambda uc,sp:(0,0)})
p.freeze_hooks();put(0x62d55c,game);put(game+0x19f04,cells)
rng=random.Random(0x50ea58)
for case in range(256):
    width,height=rng.randrange(2,17)*2,rng.randrange(2,17)*2
    heights=[rng.randrange(256) for _ in range(width*height)]
    if case<8:heights=[case*32]*(width*height)
    data=bytearray(width*height*14)
    for i,h in enumerate(heights):data[i*14+4]=h
    p.uc.mem_write(cells,bytes(data));put(game+0x19e98,width);put(game+0x19e9c,height)
    put(game+0x19f08,0);p.uc.reg_write(UC_X86_REG_EBP,frame+0x100)
    _,error=p.call(0x50ea58,());assert not error,error
    w,h=width//2,height//2
    pairs=[[0,255] for _ in range((w*h+7)&~7)]
    def update(index,value):
        if index is not None:
            pairs[index]=[max(pairs[index][0],value)&255,min(pairs[index][1],value)&255]
    for x in range(width):
        left=right=None
        for z in range(height):
            elevation=heights[z*width+x]
            projected=z*16-(elevation>>1);row=projected>>5
            if row>=0:
                edge=(row*32+31)*elevation//(projected+31)
                update(left,edge);update(right,edge)
                a,b=(x-1)>>1,x>>1
                left=row*w+a if 0<=a<w and row<h else None
                right=row*w+b if a!=b and 0<=b<w and row<h else None
                update(left,edge);update(right,edge)
            update(left,elevation);update(right,elevation)
    expected=bytes(v for pair in pairs for v in pair)
    actual=bytes(p.uc.mem_read(out,len(expected)))
    assert actual==expected,(case,width,height,[(i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b][:12])
print('PASS: 256 native visibility height-pair maps match projected terrain accumulation and boundary interpolation')
