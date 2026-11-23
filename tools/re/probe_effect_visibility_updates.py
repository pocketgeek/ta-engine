#!/usr/bin/env python3
"""Observe native current-visibility counts versus retained mapping bits.

Runs 4c6800 with overlapping small revealers. Their 3x3 footprint takes the
native near-center branch, so this does not establish distant LOS geometry.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
game,player,first,second,counts,mapping=[HEAP+i*0x20000 for i in range(6)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(0x62d55c,game);put(game+0x19e98,128);put(game+0x19e9c,96)
put(game+0x19ef4,mapping);p.uc.mem_write(game+0x306f,b'\xff')
put(player+0x88,counts);put(player+0x8c,64);put(player+0x90,48)
rng=random.Random(0x4c6800)
config=HEAP+0xc0000
put(0x62d558,config);put(config+8,config+0x100)
p.uc.mem_write(config+0x115,b'\x01\x01')
put(game+0x19f0c,64);put(game+0x19f10,48)
for case in range(256):
    who=rng.randrange(10);x,z=rng.randrange(2,60),rng.randrange(2,46)
    p.uc.mem_write(player+0xeb,bytes([who]))
    initial=rng.getrandbits(16)
    p.uc.mem_write(counts,bytes(64*48))
    p.uc.mem_write(mapping,struct.pack('<H',initial)*(64*48))
    for obj in (first,second):
        p.uc.mem_write(obj,bytes(32));put(obj,player)
        p.uc.mem_write(obj+0x14,struct.pack('<hhihBB',x,z,0,16,1,0))
    cells={zz*64+xx for zz in range(z-1,z+2) for xx in range(x-1,x+2)}
    for obj,enable,explore,level in ((first,1,1,1),(first,1,1,1),
            (second,1,1,2),(first,0,0,1),(second,0,0,0)):
        _,error=p.call(0x4c6800,(enable,explore),ecx=obj);assert not error,error
        actual=bytes(p.uc.mem_read(counts,64*48))
        words=struct.unpack('<'+'H'*(64*48),p.uc.mem_read(mapping,64*48*2))
        assert all(v==(level if i in cells else 0) for i,v in enumerate(actual)),(case,level)
        assert all(v==(initial|(1<<who) if i in cells else initial) for i,v in enumerate(words)),case
    # Real refresh removes the old stamp and installs the new one. Its center
    # uses raw whole X/Z divided by 32; height enters the eye value separately.
    for moved_x in (x+1,x+2):
        p.uc.mem_write(first+8,struct.pack('<3i',moved_x*32*65536,64*65536,z*32*65536))
        _,error=p.call(0x4c6a70,(),ecx=first);assert not error,error
        assert struct.unpack('<hhI',p.uc.mem_read(first+0x14,8))==(moved_x,z,65)
        expected_cells={zz*64+xx for zz in range(z-1,z+2) for xx in range(moved_x-1,moved_x+2)}
        actual=bytes(p.uc.mem_read(counts,64*48))
        assert all(v==int(i in expected_cells) for i,v in enumerate(actual)),case
print('PASS: 256 native overlapping-revealer lifecycles reference-count current visibility and retain explored player bits after removal')
print('PASS: 512 native refreshes remove old stamps, retain raw X/Z centers and add authored eye height to unit Y')
