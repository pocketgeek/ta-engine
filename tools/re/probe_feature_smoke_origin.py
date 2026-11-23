#!/usr/bin/env python3
"""Observe native burning-feature smoke origin and request parameters.

Executes 495ce3..495e2e. Feature-position lookup and current body frame are
inputs; CRT rolls and final smoke insertion are sinks. Existing emitter avoids
allocator setup. Admission cadence is outside this block.
"""
import random
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import *
p=Icd();feature,definition,frame,emitter=[HEAP+i*0x10000 for i in range(4)]
base=STACK+0x8000;origin=(0,0,0);rolls=[];calls=[]
def position(uc,sp):
    out,where,kind=struct.unpack('<3I',uc.mem_read(sp,12))
    assert (where,kind)==(feature+0x28,definition)
    uc.mem_write(out,struct.pack('<3I',*origin));return 3,out
def random_roll(uc,sp):return 0,rolls.pop(0)
def emit(uc,sp):
    count,point,period,small,steam=struct.unpack('<5I',uc.mem_read(sp,20))
    calls.append((count,struct.unpack('<3I',uc.mem_read(point,12)),period,small,steam))
    assert uc.reg_read(UC_X86_REG_ECX)==emitter
    return 5,0
p.hooks.update({0x4931e0:position,0x536400:lambda uc,sp:(1,frame),
                0x5d4444:random_roll,0x4f1d40:emit});p.freeze_hooks()
p.uc.mem_write(feature+0x38,struct.pack('<I',emitter))
rng=random.Random(0x495ce3)
for case in range(2048):
    width,height=[rng.randrange(1,1025) for _ in range(2)]
    ax,ay=[rng.randrange(-1024,1025) for _ in range(2)]
    origin=tuple(rng.randrange(0x100000000) for _ in range(3))
    rx,ry=[rng.randrange(32768) for _ in range(2)];rolls[:]=[rx,ry]
    p.uc.mem_write(frame,struct.pack('<HHhh',width,height,ax,ay))
    p.uc.mem_write(base-0xc,struct.pack('<I',definition))
    for reg,value in ((UC_X86_REG_EBP,base),(UC_X86_REG_ESP,base-0x100),
                      (UC_X86_REG_ESI,feature),(UC_X86_REG_EDI,definition)):
        p.uc.reg_write(reg,value)
    calls.clear();p.uc.emu_start(0x495ce3,0x495e2e)
    dx=width//4+rx*(width//2)//32768-ax
    dy=2*(ay-ry*(height//2)//32768-height//4)
    expected=((origin[0]+(dx<<16))&0xffffffff,(origin[1]+(dy<<16))&0xffffffff,origin[2])
    assert calls==[(1,expected,0,0,0)] and not rolls,(case,calls,expected,rolls)
print('PASS: 2048 native feature smoke requests use current burn-frame dimensions/anchors, two CRT rolls, fixed-point origin and one ordinary smoke particle')
