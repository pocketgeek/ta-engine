#!/usr/bin/env python3
"""Observe native storm draw ordering and projection with controlled sinks.

Terrain lookup, visibility, frame lookup and rasterization are recorded sinks;
this checks their admission/arguments, not their internal implementations.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
game, shot = HEAP, HEAP + 0x20000
events = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def terrain(uc, sp):
    assert read(sp) == shot + 4
    events.append(('terrain',))
    return 1, height & 0xffffffff


def visibility(uc, sp):
    events.append(('visibility', *struct.unpack('<3i', uc.mem_read(sp, 12))))
    assert read(shot + 8) == (height << 16) & 0xffffffff
    return 3, visible


def frame(uc, sp):
    assert read(sp) == shot + 0x3c
    events.append(('frame',))
    return 1, 123


def draw(uc, sp):
    events.append(('draw', *struct.unpack('<6i', uc.mem_read(sp, 24))))
    return 6, 0


p.hooks.update({0x511170: terrain, 0x540150: visibility,
                0x536400: frame, 0x4fac00: draw})
p.freeze_hooks()
put(0x62d55c, game)
rng = random.Random(0x52fd00)
for case in range(4096):
    tick, start = rng.choice(((99, 100), (100, 100), (101, 100),
                              (0xffffffff, 0), (0, 0xffffffff)))
    coords = [rng.randrange(-0x80000000, 0x80000000) for _ in range(3)]
    camera = [rng.randrange(-32768, 32768) for _ in range(2)]
    height, visible = rng.randrange(-32768, 32768), rng.randrange(2)
    for i, value in enumerate(coords):
        put(shot + 4 + i * 4, value)
    for i, value in enumerate(camera):
        put(game + 0x14ed0 + i * 4, value)
    put(game + 0x19f44, tick)
    put(shot + 0x60, start)
    events.clear()
    _, error = p.call(0x52fd00, (shot,))
    assert not error, (case, error)
    expected = []
    if tick >= start:
        x = (coords[0] >> 16) - camera[0]
        y = (coords[2] >> 16) - (height >> 1) - camera[1]
        expected = [('terrain',), ('visibility', game + 0x19e44, x, y)]
        if visible:
            expected += [('frame',), ('draw', 123, x, y, 0, 0, 0)]
    assert events == expected, (case, events, expected)
    expected_y = (height << 16) if tick >= start else coords[1]
    assert read(shot + 8) == expected_y & 0xffffffff, case
print('PASS: 4096 native storm draws match activation, terrain refresh before visibility, and whole-coordinate projection')
