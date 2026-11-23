#!/usr/bin/env python3
"""Observe native mission construction preserving supplied destination XYZ.

Descriptor lookup and unit-reference bookkeeping are controlled; the actual
mission constructor executes. This does not establish UI picking/network Y.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p=Icd()
mission,point,descriptor,game=[HEAP+i*0x1000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def reference(uc,sp):
    uc.mem_write(uc.reg_read(UC_X86_REG_ECX),bytes(12))
    return 2,0
p.hooks.update({0x519990:reference,0x5199f0:lambda uc,sp:(1,0),
                0x4d4cc0:lambda uc,sp:(0,descriptor)})
p.freeze_hooks()
put(0x62d55c,game);put(game+0x19f44,1234);put(descriptor+0x11,0x400)
rng=random.Random(0x4d6c40)
for case in range(2048):
    coords=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    p.uc.mem_write(point,struct.pack('<3i',*coords))
    p.uc.mem_write(mission,bytes(0x80))
    supplied=point if case%8 else 0
    args=(1,0,supplied,0,0,0,0,0,0,0,0,0)
    result,error=p.call(0x4d6c40,args,ecx=mission)
    assert not error,error
    assert result==mission
    expected=struct.pack('<3i',*coords) if supplied else bytes(12)
    assert bytes(p.uc.mem_read(mission+0x22,12))==expected
    p.uc.mem_write(point,bytes(12))
    assert bytes(p.uc.mem_read(mission+0x22,12))==expected
print('PASS: 2048 native mission constructors copy supplied XYZ exactly, default absent destinations to zero, and retain an independent copy')
