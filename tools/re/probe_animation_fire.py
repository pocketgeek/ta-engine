#!/usr/bin/env python3
"""Observe aim-state retirement in native projectile creation (530220).

Allocation, target lookup and the weapon's fire virtual are controlled sinks.
The real post-fire flags, mana deduction and projectile record initialization
execute. This does not verify all weapon subclasses or authoritative firing.
"""
import random
import struct
from emu import Icd,HEAP
p=Icd()
unit,kind,record,weapon,projectile,game,vtable,virtual=[HEAP+i*0x10000 for i in range(8)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def short(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
def word(a):return struct.unpack('<H',p.uc.mem_read(a,2))[0]
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
operation=0
seen=[]
allocated=True

def fire(uc,sp):
    args=struct.unpack('<3I',uc.mem_read(sp,12))
    assert args==(projectile,unit,record),args
    flags=word(record+0x1a)
    seen.append(flags)
    # Model the synchronous script SET acknowledged by the fire callback.
    if operation==21: flags &= 0xff07
    elif operation==22: flags |= 8
    elif operation==23: flags |= 16
    short(record+0x1a,flags)
    return 3,0

p.hooks.update({0x529a90:lambda uc,sp:(0,projectile if allocated else 0),
                0x51a9a0:lambda uc,sp:(2,0),virtual:fire})
p.freeze_hooks()
put(0x62d55c,game);put(game+0x19f44,100)
put(unit+0xb4,kind);put(record,weapon);put(weapon,vtable);put(vtable+0x2c,virtual)
p.uc.mem_write(unit+0xf4,struct.pack('<f',1.0))
p.uc.mem_write(weapon+0xd4,struct.pack('<f',2.0))
short(weapon+0xbe,0) # no recoil callback
rng=random.Random(0x5302c0)
for i in range(4096):
    flags=rng.randrange(65536);operation=rng.choice((0,21,22,23))
    allocated=i%7!=0
    short(record+0x1a,flags);seen.clear()
    p.uc.mem_write(unit+0xd8,struct.pack('<f',10.0))
    _,error=p.call(0x530220,(unit,record),ecx=weapon)
    assert not error,error
    expected=flags
    if allocated:
        if operation==21: expected &= 0xff07
        elif operation==22: expected |= 8
        elif operation==23: expected |= 16
        expected &= 0xff0f
    assert seen==([flags] if allocated else [])
    assert word(record+0x1a)==expected,(i,hex(flags),operation,word(record+0x1a),expected)
    mana=struct.unpack('<f',p.uc.mem_read(unit+0xd8,4))[0]
    assert mana==(8.0 if allocated else 10.0),(i,mana)
    if allocated:
        assert read(projectile)==weapon and read(projectile+0x7c)==unit
        assert read(projectile+0x64)==100
print('PASS: 4096 native projectile creation cases; fire callback precedes aim-bit retirement and mana deduction, allocation failure preserves state')
