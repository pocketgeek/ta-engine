#!/usr/bin/env python3
"""Compare native named-lightning clipping and uploaded quad vertices.

52ccf0 executes with visibility admitted and controlled muzzle coordinates.
Only final texture upload/device calls are replaced; native clipping, direction,
rotation, length, corner construction and UV conversion execute unchanged.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f) # Retail's 53-bit precision, nearest/even.
game, weapon, shot, owner, effect, device, vtable = [HEAP + i * 0x10000 for i in range(7)]
muzzle = (0, 0, 0)
draws, states = [], []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def query(uc, sp):
    assert read(sp) == owner and read(sp + 4) == shot + 0x10
    uc.mem_write(shot + 0x10, struct.pack('<3i', *muzzle))
    return 4, 0


def state(uc, sp):
    states.append(struct.unpack('<2I', uc.mem_read(sp, 8)))
    return 2, 0


def draw(uc, sp):
    kind, vertices, count = struct.unpack('<3I', uc.mem_read(sp, 12))
    assert (kind, count) == (6, 4)
    result = []
    for index in range(4):
        values = struct.unpack('<8I', uc.mem_read(vertices + index * 32, 32))
        assert values[2:5] == (0, 0x3f800000, 0xffffffff)
        result.extend((values[0], values[1], values[6], values[7]))
    draws.append(tuple(result))
    return 3, 0


p.hooks.update({0x4dd420: query, 0x48c870: lambda uc, sp: (1, 1),
                0x531f70: lambda uc, sp: (2, 1), 0x4f4e70: lambda uc, sp: (0, 0),
                0x5ac2f0: lambda uc, sp: (0, device), 0x5ac370: lambda uc, sp: (0, device),
                0x5ac3a0: lambda uc, sp: (0, 0), HEAP + 0x70000: state, HEAP + 0x70010: draw})
p.freeze_hooks()
put(0x62d55c, game)
put(game + 0x19f44, 1)
put(shot, weapon)
put(shot + 0x7c, owner)
put(shot + 0xa4, effect)
put(effect + 0x63e, 123)
put(device, vtable)
put(vtable + 0x6c, HEAP + 0x70000)
put(vtable + 0x64, HEAP + 0x70010)
rng = random.Random(0x52cfb1)
rows, expected = [], []
for case in range(4096):
    x0, y0, x1, y1 = (rng.randrange(-4000, 6001) for _ in range(4))
    if case % 8 == 0:
        x1 = x0
    if case % 8 == 1:
        y1 = y0
    vw, vh = rng.randrange(320, 3001), rng.randrange(240, 1801)
    width, height = rng.randrange(1, 513), rng.randrange(1, 129)
    tw, th = 1 << (width - 1).bit_length(), 1 << (height - 1).bit_length()
    for offset, value in ((4, width), (8, height), (12, tw), (16, th)):
        put(effect + offset, value)
    put(game + 0x19e30, vw)
    put(game + 0x19e34, vh)
    muzzle = (x0 * 65536, 0, y0 * 65536)
    p.uc.mem_write(shot + 4, struct.pack('<3i', x1 * 65536, 0, y1 * 65536))
    draws.clear()
    states.clear()
    _, error = p.call(0x52ccf0, (shot,), ecx=weapon)
    assert not error, (case, error)
    assert len(draws) <= 1
    if draws:
        assert (1, 123) in states and (0x13, 5) in states and (0x14, 6) in states
    expected.append((1, *draws[0]) if draws else (0,))
    rows.append(' '.join(map(str, (x0, y0, x1, y1, vw, vh, width, height, tw, th))))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
result = subprocess.run([binary, '--lightning-quad'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((i, rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b), 'row count')
print('PASS: 4096 native lightning quads match clipping/rejection, vertex order, rounded '
      'corners and padded-texture UV float bits; native blend-state dispatch also checked')
