#!/usr/bin/env python3
"""Observe native construction particle projection and front/back selection.

Native traversal, projection, frame lookup and 4fac00 execute together.
Renderer state setters and primitive submission are sinks; no pixel-parity claim.
"""
import random
import struct
from emu import Icd,HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_FPCW
p=Icd();game,emitter,head,node,owner,animation=[HEAP+i*0x20000 for i in range(6)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
put(0x62d55c,game);put(emitter+8,head);put(head,node);put(node,head);put(node+8,owner)
device,vtable,api,texture=HEAP+0x200000,HEAP+0x210000,HEAP+0x220000,HEAP+0x230000
put(device,vtable);put(vtable+0x6c,api);put(vtable+0x64,api+16)
put(vtable+0x50,api+32);put(vtable+0x54,api+48)
put(texture,123,64,64)
state={};draws=[]
def setting(uc,sp):
    k,v=struct.unpack('<II',uc.mem_read(sp,8));state[k]=v;return 2,0
def quad(uc,sp):
    primitive,vertices,count=struct.unpack('<III',uc.mem_read(sp,12))
    draws.append((primitive,count,dict(state)));return 3,0
seen=[]
def sprite(uc,sp):
    seen.append(struct.unpack('<IiiIII',uc.mem_read(sp,24)))
    return 6,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),0x5ac370:lambda uc,sp:(0,device),
                0x5ac3a0:lambda uc,sp:(0,0),api:setting,api+16:quad,
                api+32:lambda uc,sp:(1,0),api+48:lambda uc,sp:(1,0)})
def observe(uc,address,size,data):
    sprite(uc,uc.reg_read(UC_X86_REG_ESP)+4)
p.uc.hook_add(UC_HOOK_CODE,observe,begin=0x4fac00,end=0x4fac00)
p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
p.freeze_hooks();rng=random.Random(0x4f1540)
def high(v):return struct.unpack('<h',struct.pack('<H',((v&0xffffffff)>>16)))[0]
cases=0
for i in range(1024):
    body=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    offsets=[rng.randrange(-0x1000000,0x1000000) for _ in range(3)]
    if i%4==0:offsets[2]=0
    camera=[rng.randrange(-1000,1000) for _ in range(2)]
    put(owner+0x68,*body);put(node+0xc,*offsets);put(game+0x14ed0,*camera)
    frame=i%12
    p.uc.mem_write(node+0x20,struct.pack("<H",frame));put(node+0x28,animation)
    for f in range(12):
        address=HEAP+0xe0000+f*64
        put(animation+0x28+f*8,address)
        p.uc.mem_write(address,struct.pack('<4h',42,45,19,21))
        p.uc.mem_write(address+9,bytes([4,0,0]));put(address+12,texture)
    xyz=[high(a+b) for a,b in zip(body,offsets)]
    expected=(HEAP+0xe0000+frame*64,xyz[0]-camera[0],xyz[2]-(xyz[1]>>1)-camera[1],0,0,0)
    for address,draw in [(0x4f1540,offsets[2]<0),(0x4f15f0,offsets[2]>=0)]:
        seen.clear();draws.clear();_,error=p.call(address,(),ecx=emitter)
        assert not error,error
        assert seen==([expected] if draw else []),(i,address,seen,expected,draw)
        assert len(draws)==int(draw)
        if draw:
            primitive,count,settings=draws[0]
            assert (primitive,count)==(6,4)
            assert (settings[19],settings[20],settings[27])==(5,2,1)
        cases+=1
print(f'PASS: composed sprite backend; {cases} native construction draw cases; signed Z partition, owner-relative wrapped projection, native per-particle frame resource lookup')
