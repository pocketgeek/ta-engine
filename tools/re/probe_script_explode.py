#!/usr/bin/env python3
"""Observe COB EXPLODE dispatch in the installed retail executable.

Only random draws, piece-position lookup, and downstream effect creation are
sinks. This verifies dispatch descriptors, not debris motion or rendered output.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
vm, view, unit, settings, options = [HEAP+i*0x10000 for i in range(5)]
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def words(address, count):
    return struct.unpack('<'+'I'*count, p.uc.mem_read(address, count*4))
put(vm+0xa64, view)
put(view+0xc, unit)
put(0x62d558, settings)
put(settings+0x18, options)
rng = random.Random(0x50dd20)
draws, debris, effects, positions = [], [], [], []
origin = (0x12345678, 0x87654321, 0xffff1111)
def random_draw(uc, sp):
    bound, = words(sp, 1)
    value = rng.randrange(bound)
    draws.append((bound, value))
    return 1, value
def create_debris(uc, sp):
    address, = words(sp, 1)
    debris.append(words(address, 11))
    return 1, 0
def position(uc, sp):
    output, owner, piece = words(sp, 3)
    positions.append((owner, piece))
    uc.mem_write(output, struct.pack('<3I', *origin))
    return 3, output
def effect(uc, sp):
    address, kind, light = words(sp, 3)
    effects.append((words(address, 3), kind, light))
    return 3, 0
p.hooks.update({0x535cc0:random_draw, 0x492cc0:create_debris,
                0x4dd250:position, 0x492c80:effect})
p.freeze_hooks()
rows, native = [], []
for case in range(4096):
    flags = case if case < 128 else rng.getrandbits(17)
    piece = case % 37
    special_enabled = case % 2
    p.uc.mem_write(options+0x11, bytes([special_enabled]))
    draws.clear(); debris.clear(); effects.clear(); positions.clear()
    _, error = p.call(0x50dd20, (piece, flags), ecx=vm)
    assert not error, (case, error)
    if flags & 0x20:
        assert not draws and not debris
    else:
        assert [bound for bound, _ in draws] == [40, 10, 40]
        assert len(debris) == 1
        descriptor = debris[0]
        velocity = ((20-draws[0][1]) << 12,
                    draws[1][1] << 14, (20-draws[2][1]) << 12)
        assert descriptor[:5] == (unit, piece, 0, 0, 0)
        assert descriptor[5:8] == tuple(v & 0xffffffff for v in velocity)
        assert descriptor[8:10] == (900, flags & 1)
        # Other high bits in this stack word are not initialized by dispatch.
        expected = ((4 if flags & 1 else 0) |
                    ((32 if flags & 1 else 16) if flags & 2 else 0) |
                    (8 if flags & 4 else 0) | (2 if flags & 8 else 0) |
                    (1 if flags & 16 else 0) | (64 if flags & 64 else 0))
        assert descriptor[10] & 0x7f == expected, (flags, descriptor)
    expected_effects = [(origin, i, i if i < 3 else 2 if i == 3 else 0xffffffff)
                        for i in range(9) if flags & (0x100 << i)
                        and (i != 4 or special_enabled)]
    assert effects == expected_effects, (flags, effects, expected_effects)
    assert positions == ([(unit, piece)] if flags & 0x1ff00 else [])
    values = [value for _, value in draws] if draws else [0, 0, 0]
    rows.append(' '.join(map(str, [flags, *values])))
    signed = lambda value: value if value < 0x80000000 else value-0x100000000
    native.append((1, 3, *(signed(v) for v in debris[0][5:8]), 900,
                   debris[0][10]&0x7f) if debris else (0, 0, 0, 0, 0, 900, 0))
print('PASS: 4096 native EXPLODE descriptors: debris suppression, velocities, flag mapping, nine independent effect bits and conditional class 4')
if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1], '--debris-launch'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    assert actual==native
    print('PASS: 4096 compiled debris launches match native velocities, mapped flags and draw counts')
