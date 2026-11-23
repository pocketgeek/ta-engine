#!/usr/bin/env python3
"""Observe native construction particle projection and front/back selection.

Renderer state and sprite submission are sinks; native list traversal,
partition and wrapped projection execute. Does not prove Glide pixel parity.
"""
import random
import struct
from emu import Icd,HEAP
p=Icd();game,emitter,head,node,owner,animation=[HEAP+i*0x20000 for i in range(6)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
put(0x62d55c,game);put(emitter+8,head);put(head,node);put(node,head);put(node+8,owner)
seen=[]
def sprite(uc,sp):
    seen.append(struct.unpack('<IiiIII',uc.mem_read(sp,24)))
    return 6,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,0),0x5ac370:lambda uc,sp:(0,0),
                0x5ac3a0:lambda uc,sp:(0,0),0x4fac00:sprite})
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
    for f in range(12):put(animation+0x28+f*8,HEAP+0xe0000+f*64)
    xyz=[high(a+b) for a,b in zip(body,offsets)]
    expected=(HEAP+0xe0000+frame*64,xyz[0]-camera[0],xyz[2]-(xyz[1]>>1)-camera[1],0,0,0)
    for address,draw in [(0x4f1540,offsets[2]<0),(0x4f15f0,offsets[2]>=0)]:
        seen.clear();_,error=p.call(address,(),ecx=emitter)
        assert not error,error
        assert seen==([expected] if draw else []),(i,address,seen,expected,draw)
        cases+=1
print(f'PASS: {cases} native construction draw cases; signed Z partition, owner-relative wrapped projection, native per-particle frame resource lookup')
