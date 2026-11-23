#!/usr/bin/env python3
"""Observe burn completion's removal/replacement protocol in 495300.

Terrain lookup, removal and placement are sinks; their internal occupancy and
network effects are not emulated here. Tests the native caller's decisions.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd();game,definitions,state,cell=[HEAP+i*0x10000 for i in range(4)]
events=[];found=True
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v))
def lookup(uc,sp):
    events.append(('lookup',*struct.unpack('<2i',uc.mem_read(sp,8))))
    return 2,cell if found else 0
def remove(uc,sp):
    events.append(('remove',*struct.unpack('<2I',uc.mem_read(sp,8))))
    return 2,0
def place(uc,sp):
    events.append(('place',*struct.unpack('<5I',uc.mem_read(sp,20))))
    return 5,0
p.hooks.update({0x50e600:lookup,0x496380:remove,0x495360:place});p.freeze_hooks()
put(0x62d55c,game);put(game+0x19edc,definitions)
rng=random.Random(0x495300)
for case in range(512):
    x,z=[rng.randrange(-32768,32768) for _ in range(2)]
    original=rng.randrange(100);replacement=0xffff if case%3==0 else rng.randrange(65535)
    found=case%4!=0
    p.uc.mem_write(state+0x28,struct.pack('<hhH',x,z,original))
    p.uc.mem_write(definitions+original*0x140+0x12e,struct.pack('<H',replacement))
    events.clear();_,error=p.call(0x495300,(state,));assert not error,error
    expected=[('lookup',x,z)]
    if found:
        expected.append(('remove',cell,0))
        # Caller writes DI only; placement masks the incoming type to 16 bits.
        if replacement!=0xffff:expected.append(('place',cell,(state & 0xffff0000)|replacement,0,0,10))
    assert events==expected,(case,events,expected)
print('PASS: 512 native burn retirements remove the original and place featureburnt, omit absent replacements, and ignore missing cells')
