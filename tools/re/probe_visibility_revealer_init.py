#!/usr/bin/env python3
"""Execute native revealer initialization with no substituted routines."""
import random
import struct
from emu import Icd, HEAP
p=Icd();p.freeze_hooks()
game,unit,kind,owner,revealer=[HEAP+i*0x20000 for i in range(5)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
put(0x62d55c,game);put(unit+0xb4,kind);put(unit+0xb8,owner)
rng=random.Random(0x4c66b0)
for case in range(2048):
    local,side=rng.randrange(10),rng.randrange(10)
    allied=rng.randrange(2);eye=rng.randrange(256);sight=rng.randrange(65536)
    coords=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    p.uc.mem_write(game+0x306e,bytes([local]));p.uc.mem_write(unit+0xfd,bytes([side]))
    local_data=game+0x2404+local*0x110
    p.uc.mem_write(local_data+0xf7+side,bytes([allied]))
    p.uc.mem_write(unit+0x68,struct.pack('<3i',*coords))
    p.uc.mem_write(kind+0x14c,bytes([eye]));p.uc.mem_write(kind+0x226,struct.pack('<H',sight))
    p.uc.mem_write(revealer,b'\xaa'*32)
    _,error=p.call(0x4c66b0,(unit,),ecx=revealer);assert not error,error
    assert read(revealer)==(local_data if allied else owner)
    assert read(revealer+4)==0
    assert struct.unpack('<3i',p.uc.mem_read(revealer+8,12))==tuple(coords)
    assert struct.unpack('<hhI',p.uc.mem_read(revealer+0x14,8))==(-1,-1,0)
    assert struct.unpack('<HBB',p.uc.mem_read(revealer+0x1c,4))==(sight,eye,0)
print('PASS: 2048 native revealers retain raw unit XYZ, authored sight/eye values, alliance routing and initial inactive state')
