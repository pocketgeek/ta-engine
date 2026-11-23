#!/usr/bin/env python3
"""Observe native display combat callbacks using a controlled local unit table.

This verifies callback argument conventions, not weapon aiming geometry or timing.
No executable bytes or assets are written by this probe.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
game, units, kind, packet = [HEAP + i * 0x10000 for i in range(4)]
unit = units + 312
calls = []
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def capture(uc, sp):
    args = struct.unpack('<8I', uc.mem_read(sp, 32))
    name = bytes(uc.mem_read(args[0], 64)).split(b'\0')[0].decode('ascii')
    calls.append((name, args[3], args[4:4 + args[3]]))
    return 8, 0
packets=[]
def send(uc,sp):
    stream,data,size=struct.unpack('<3I',uc.mem_read(sp,12))
    assert stream==42 and size==6
    packets.append(bytes(uc.mem_read(data,size)))
    return 3,0
p.hooks[0x4e8030]=send
p.hooks[0x56c640] = capture
p.freeze_hooks()
put(0x62d55c, game)
put(game + 0x14e84, units)
put(game + 0x14e88, unit)
put(unit + 0x130, 0x1000000)
put(unit + 0xb4, kind)
rng = random.Random(0x4ea5c0)
count = 0
for slot in range(3):
    for _ in range(256):
        heading, pitch = rng.randrange(256), rng.randrange(256)
        p.uc.mem_write(packet, struct.pack('<BHBBB', 0, 1, heading, pitch, slot))
        calls.clear()
        _, error = p.call(0x4ea5c0, (packet,))
        assert not error, error
        assert calls == [('AimWeapon', 3, (heading << 8, pitch << 8, slot))], calls
        count += 1
    p.uc.mem_write(packet, struct.pack("<BHB", 0, 1, slot))
    calls.clear()
    _, error = p.call(0x4ea640, (packet,))
    assert not error, error
    assert calls == [("FireWeapon", 1, (slot,))], calls
    # A target that was present generates a clear event; repeating it is silent.
    put(unit + 0xc + slot * 28 + 4, 1)
    calls.clear()
    _, error = p.call(0x51a7f0, (unit, slot))
    assert not error, error
    assert calls == [('TargetCleared', 1, (slot,))], calls
    calls.clear()
    _, error = p.call(0x51a7f0, (unit, slot))
    assert not error, error
    assert not calls, calls
print(f'PASS: {count} native AimWeapon argument cases, 3 FireWeapon slots and 6 target-clear transitions')

# Target clearing retires a reference in every slot, but callback dispatch is
# restricted to the selected slot when the armed unit is not in all-slot mode.
count=0
for enabled in (False,True):
    put(kind+0x260,0x10000 if enabled else 0)
    for selected in range(4):
        put(unit+0x130,0x1000000|(selected<<30))
        for slot in range(3):
            for target_id,height in ((0,0x8000),(1,0x8000),(0,123),(65535,0)):
                record=unit+12+slot*28
                p.uc.mem_write(record+4,struct.pack('<2H',target_id,height))
                p.uc.mem_write(record+0x1a,struct.pack('<H',0xe8|slot))
                calls.clear()
                _,error=p.call(0x51a7f0,(unit,slot));assert not error,error
                present=target_id!=0 or height!=0x8000
                expected=present and (not enabled or selected==3 or selected==slot)
                assert calls==([('TargetCleared',1,(slot,))] if expected else []),(enabled,selected,slot,target_id,height,calls)
                assert bytes(p.uc.mem_read(record+4,4))==struct.pack('<2H',0,0x8000)
                assert struct.unpack('<H',p.uc.mem_read(record+0x1a,2))[0]==0xe8|slot
                count+=1
put(kind+0x260,0);put(unit+0x130,0x1000000)
print(f'PASS: {count} native target-clear selection/reference combinations; clearing alone preserves aim flags')

# Exercise the native sender as well as decoder: the low eight angle bits
# disappear in the display packet, while the authoritative aim keeps them.
player=HEAP+0x40000
put(game+0x3070,1);put(unit+0xb8,player);put(player,0);put(player+4,42)
p.uc.mem_write(unit+2,struct.pack('<H',1))
rows=[];expected=[]
for slot in range(3):
    for _ in range(1024):
        heading,pitch=rng.randrange(65536),rng.randrange(65536)
        packets.clear();calls.clear()
        _,error=p.call(0x4ea4f0,(unit,heading,pitch,slot));assert not error,error
        assert packets==[struct.pack('<BHBBB',15,1,heading>>8,pitch>>8,slot)]
        p.uc.mem_write(packet,packets[0])
        _,error=p.call(0x4ea5c0,(packet,));assert not error,error
        angles=(heading&0xff00,pitch&0xff00,slot)
        assert calls==[('AimWeapon',3,angles)],calls
        rows.append(f'{heading} {pitch} {slot}');expected.append(angles)
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--weapon-animation'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected
print(f'PASS: {len(rows)} native aim packet round trips match authoritative display-event angles and slots')
