#!/usr/bin/env python3
"""Observe native emitter sizing slice; no routine substitutions.

Executes 4ee377..4ee46f with synthetic type bounds and quality settings.
Does not establish bounds provenance or outer render-object lifecycle.
"""
import math
import struct
from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EIP, UC_X86_REG_FPCW
p=Icd();p.freeze_hooks()
owner,kind,render=HEAP,HEAP+0x1000,HEAP+0x2000
bp=STACK+STACK_SZ-0x1000

def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def get(a):return struct.unpack('<i',p.uc.mem_read(a,4))[0]
put(owner+0xb4,kind);put(render+0xc,owner)
cases=0
for dx,dz,dy in [(0,0,0),(2,2,3),(32,48,64),(127,9,81),(13.5,22.25,6.5),(256,128,96)]:
 for special in (False,True):
  for mobile in (0,1,2):
   for quality,current,target in [(0,0,16),(1,0,1),(1,0,2),(1,0,5),(1,0,16),(1,8,4)]:
    for offset,extent in [(0x13a,dx),(0x13e,dy),(0x142,dz)]:
     put(kind+offset,-7*65536);put(kind+offset+12,int((extent-7)*65536))
    p.uc.mem_write(owner+0x133,bytes([2 if special else 0]))
    p.uc.mem_write(kind+0x24a,bytes([mobile]))
    p.uc.mem_write(0x61a01c,bytes([quality]));put(0x61a024,current);put(0x61a02c,target)
    for reg,value in [(UC_X86_REG_EBP,bp),(UC_X86_REG_ESP,bp-0x100),(UC_X86_REG_EBX,owner),(UC_X86_REG_EDI,render),(UC_X86_REG_FPCW,0x37f)]:p.uc.reg_write(reg,value)
    p.uc.emu_start(0x4ee377,0x4ee46f,timeout=1000000)
    assert p.uc.reg_read(UC_X86_REG_EIP)==0x4ee46f
    hx=int(dx*65536)//2;hz=int(dz*65536)//2
    radius=min(hx,hz) if special else round(math.sqrt(hx*hx+hz*hz))
    capacity=radius>>16
    if mobile==1:capacity//=4
    if quality and current+1<target:capacity=max(1,capacity//min(6,(target-current)//2))
    actual=(get(bp-4),get(bp+0x10),p.uc.reg_read(UC_X86_REG_ESI))
    assert actual==(radius,capacity,int(dy*65536)),(dx,dz,special,mobile,quality,current,target,actual,radius,capacity)
    cases+=1
print(f'PASS: {cases} native emitter sizing cases; model half-extents, radius branch, mobile capacity and quality reduction')
