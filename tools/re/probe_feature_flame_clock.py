#!/usr/bin/env python3
"""Observe native burning-feature clock updates and completion choice.

Executes 495e31..495f0f with actual animation clocks. Final retirement is a sink;
smoke emission, ignition setup, spread timers and replacement are outside scope.
"""
import random
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import *
p=Icd()
feature,definition,body,front,back=[HEAP+i*0x10000 for i in range(5)]
shadow,reference=HEAP+0x50000,HEAP+0x60000
base=STACK+0x8000
retired=[];spreads=[]
def retire(uc,sp):
    retired.append(struct.unpack('<I',uc.mem_read(sp,4))[0]);return 1,0
def spread(uc,sp):
    spreads.append(struct.unpack('<2I',uc.mem_read(sp,8)));return 2,0
p.hooks[0x495300]=retire;p.hooks[0x495110]=spread;p.freeze_hooks()
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v))
rng=random.Random(0x495e31);checks=0
for case in range(256):
    present=case%4
    durations=[[rng.randrange(0,6) for _ in range(rng.randrange(1,6))] for _ in range(3)]
    p.uc.mem_write(feature,bytes(0x60));p.uc.mem_write(definition,bytes(0x140))
    put(definition+0x108,front if present&1 else 0)
    put(definition+0x10c,back if present&2 else 0)
    for index,(animation,offset) in enumerate(((body,4),(front,0x45),(back,0x51))):
        ds=durations[index];data=bytearray(0x30+8*len(ds))
        struct.pack_into('<HB',data,0,len(ds),int(index==0 and present!=0))
        for i,duration in enumerate(ds):struct.pack_into('<H',data,0x2c+8*i,duration)
        p.uc.mem_write(animation,bytes(data))
        if index==0 or present & (1<<(index-1)):
            _,error=p.call(0x537390,(feature+offset,animation,0));assert not error,error
    # Compare the instance shadow against an independently advanced native clock.
    shadow_data=bytearray(0x40)
    struct.pack_into('<HB',shadow_data,0,2,1)
    struct.pack_into('<H',shadow_data,0x2c,2)
    struct.pack_into('<H',shadow_data,0x34,3)
    p.uc.mem_write(shadow,bytes(shadow_data))
    for address in (feature+0x10,reference):
        _,error=p.call(0x537390,(address,shadow,0));assert not error,error
    length=lambda ds:sum(max(1,d) for d in ds)
    end=max([length(durations[i+1]) for i in range(2) if present&(1<<i)],default=length(durations[0]))
    spark=max(1,end+(case%3)-1)
    suppressed=case%5==0
    p.uc.mem_write(feature+0x44,bytes((spark,)))
    p.uc.mem_write(feature+0x5d,bytes((4 | (8 if suppressed else 0),)))
    for age in range(1,end+1):
        for reg,value in ((UC_X86_REG_EBP,base),(UC_X86_REG_ESP,base-0x100),
                          (UC_X86_REG_ESI,feature),(UC_X86_REG_EDI,definition)):
            p.uc.reg_write(reg,value)
        retired.clear();spreads.clear();p.uc.emu_start(0x495e31,0x495f0f)
        assert retired==([feature] if age==end else []),(case,age,end,retired)
        assert spreads==([(definition,feature+0x28)] if age==spark and age<end and not suppressed else []),(case,age,end,spark,spreads)
        if not present:
            _,error=p.call(0x5373d0,(reference,));assert not error,error
        assert p.uc.mem_read(feature+0x10,12)==p.uc.mem_read(reference,12),(case,age,'shadow clock')
        checks+=1
print(f'PASS: {checks} native burn updates across 256 timelines retire after both overlay clocks end, or the body clock when neither overlay exists')

print('PASS: spread fires only before retirement, never on the expiry tick; native suppression flag also prevents it')

print('PASS: burn shadow clock advances without overlays and remains frozen when either flame overlay exists')
