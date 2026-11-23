#!/usr/bin/env python3
"""Compare native feature-smoke body-frame reads with pre-update clock age.

Executes the real 495ce3..495e2e smoke block and 5373d0 clock update. Only
feature world-position lookup, CRT rolls, and final particle insertion are
controlled. This tests sampling phase after a synthetic clock start, not the
game-level ignition/update boundary.
"""
import random
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import *

p=Icd()
feature,definition,sequence,emitter=[HEAP+i*0x10000 for i in range(4)]
base=STACK+0x8000
origin=(0x00100000,0x00200000,0x00300000)
rolls=[];requests=[]

def position(uc,sp):
    out,where,kind=struct.unpack('<3I',uc.mem_read(sp,12))
    assert (where,kind)==(feature+0x28,definition)
    uc.mem_write(out,struct.pack('<3I',*origin))
    return 3,out

def random_roll(uc,sp):
    return 0,rolls.pop(0)

def emit(uc,sp):
    count,point,period,small,steam=struct.unpack('<5I',uc.mem_read(sp,20))
    requests.append((count,struct.unpack('<3I',uc.mem_read(point,12)),period,small,steam))
    assert uc.reg_read(UC_X86_REG_ECX)==emitter
    return 5,0

p.hooks.update({0x4931e0:position,0x5d4444:random_roll,0x4f1d40:emit})
p.freeze_hooks()
rng=random.Random(0x495ce3)
checks=0

def frame_at(durations,age):
    age%=sum(max(1,d) for d in durations)
    for index,duration in enumerate(durations):
        span=max(1,duration)
        if age<span:return index
        age-=span
    raise AssertionError('unreachable frame')

for case in range(128):
    count=rng.randrange(2,8)
    durations=[rng.randrange(0,8) for _ in range(count)]
    start_tick=rng.randrange(0,9)
    p.uc.mem_write(feature,bytes(0x80))
    p.uc.mem_write(definition,bytes(0x140))
    p.uc.mem_write(feature+0x38,struct.pack('<I',emitter))
    metadata=bytearray(0x30+8*count)
    struct.pack_into('<HB',metadata,0,count,1)
    headers=[]
    for index,duration in enumerate(durations):
        frame=HEAP+0x50000+index*0x100
        width,height=rng.randrange(5,128),rng.randrange(5,128)
        anchor_x,anchor_y=rng.randrange(-20,21),rng.randrange(-20,21)
        p.uc.mem_write(frame,struct.pack('<HHhh',width,height,anchor_x,anchor_y))
        headers.append((width,height,anchor_x,anchor_y))
        struct.pack_into('<I',metadata,0x28+8*index,frame)
        struct.pack_into('<H',metadata,0x2c+8*index,duration)
    p.uc.mem_write(sequence,bytes(metadata))
    _,error=p.call(0x537390,(feature+4,sequence,0));assert not error,error
    for elapsed in range(1,49):
        tick=start_tick+elapsed
        if tick%3==0:
            local_age=elapsed-1
            expected_index=frame_at(durations,local_age)
            width,height,anchor_x,anchor_y=headers[expected_index]
            rx,ry=rng.randrange(32768),rng.randrange(32768)
            rolls[:]=[rx,ry]
            p.uc.mem_write(base-0xc,struct.pack('<I',definition))
            for reg,value in ((UC_X86_REG_EBP,base),(UC_X86_REG_ESP,base-0x100),
                              (UC_X86_REG_ESI,feature),(UC_X86_REG_EDI,definition)):
                p.uc.reg_write(reg,value)
            requests.clear()
            p.uc.emu_start(0x495ce3,0x495e2e)
            dx=width//4+rx*(width//2)//32768-anchor_x
            dy=2*(anchor_y-ry*(height//2)//32768-height//4)
            expected=((origin[0]+(dx<<16))&0xffffffff,
                      (origin[1]+(dy<<16))&0xffffffff,origin[2])
            assert requests==[(1,expected,0,0,0)],(case,elapsed,durations,requests,expected)
            assert not rolls,(case,elapsed,rolls)
            checks+=1
        _,error=p.call(0x5373d0,(feature+4,));assert not error,error

print(f'PASS: {checks} native feature-smoke requests sample the burn-body frame before that tick’s 5373d0 clock advance')
print('LIMIT: synthetic start/clock phase only; ignition timing in the full world callback remains unproven')
