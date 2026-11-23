#!/usr/bin/env python3
"""Observe native beam-particle projection and sight admission.

Runs 5042e0; sinks animation lookup and final sprite submission only.
Glide blend/raster behavior is not covered by this probe.
"""
import random
import struct
from emu import Icd, HEAP
p = Icd()
particle, game, root, config, sight, explored = [HEAP + i * 0x10000 for i in range(6)]
calls = []
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def lookup(uc, sp):
    animation, frame = struct.unpack('<2I', uc.mem_read(sp, 8))
    calls.append(('frame', animation, frame))
    return 2, 12345

def draw(uc, sp):
    calls.append(('draw', *struct.unpack('<4i', uc.mem_read(sp, 16))))
    return 4, 0
p.hooks.update({0x5367a0: lookup, 0x536e90: draw})
p.freeze_hooks()
put(0x62d55c, game); put(0x62d558, root); put(root + 8, config)
put(game + 0x19ef4, explored)
player = 2
p.uc.mem_write(game + 0x306f, bytes((player,)))
view = game + 0x2404 + player * 0x110
put(view + 0x88, sight); put(view + 0x8c, 64); put(view + 0x90, 64)
put(particle, 777); put(particle + 0x2c, 3)
rng = random.Random(0x5042e0)
for case in range(2048):
    x, y, z = [rng.randrange(-100, 2300) for _ in range(3)]
    ox, oy = rng.randrange(-100, 100), rng.randrange(-100, 100)
    for i, v in enumerate((x, y, z)): put(particle + 4 + i * 4, v * 65536)
    use_sight = case % 2
    visible = case % 3 != 0
    p.uc.mem_write(config + 0x15, bytes((use_sight,)))
    p.uc.mem_write(sight, bytes((int(visible),)) * 4096)
    p.uc.mem_write(explored, struct.pack('<H', (1 << player) if visible else 0) * 4096)
    calls.clear()
    _, error = p.call(0x5042e0, (999, ox, oy), ecx=particle)
    assert not error, error
    py = z - (y >> 1)
    admitted = visible and 0 <= x >> 5 < 64 and 0 <= py >> 5 < 64
    expected = [('frame', 777, 3), ('draw', 999, 12345, x - ox, py - oy)] if admitted else []
    assert calls == expected, (case, calls, expected)
print('PASS: 2048 native script-beam draws match height projection, camera offset, frame selection and sight/explored admission')
