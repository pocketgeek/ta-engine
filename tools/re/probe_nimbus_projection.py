#!/usr/bin/env python3
"""Observe native caster nimbus draw placement; only final draw/frame are sinks."""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ESI

p = Icd()
view, unit, game = HEAP, HEAP + 0x10000, HEAP + 0x20000
calls = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def frame(uc, sp):
    assert struct.unpack('<I', uc.mem_read(sp, 4))[0] == view + 0x1a4
    return 1, 123


def draw(uc, sp):
    calls.append(struct.unpack('<6i', uc.mem_read(sp, 24)))
    return 6, 0


# Start/end delimit exactly the nimbus branch inside the unit display routine.
p.hooks.update({0x536400: frame, 0x4fac00: draw, 0x4ecd3a: lambda uc, sp: (0, 0)})
p.freeze_hooks()
put(0x62d55c, game)
put(view + 0xc, unit)
rng = random.Random(0x4eccea)
for case in range(4096):
    active = rng.randrange(2)
    coords = [rng.randrange(-32768 * 65536, 32768 * 65536) for _ in range(3)]
    camera = [rng.randrange(-32768, 32768) for _ in range(2)]
    put(view + 0x1ac, active)
    for i, value in enumerate(coords):
        put(unit + 0x68 + i * 4, value)
    for i, value in enumerate(camera):
        put(game + 0x14ed0 + i * 4, value)
    p.uc.reg_write(UC_X86_REG_ESI, view)
    calls.clear()
    _, error = p.call(0x4eccea, ())
    assert not error, (case, error)
    x, height, z = [value >> 16 for value in coords]
    expected = [(123, x - camera[0], z - (height >> 1) - camera[1], 0, 0, 0)] if active else []
    assert calls == expected, (case, calls, expected)
print('PASS: 4096 native nimbus draws match active gate and whole-coordinate caster projection (including negative heights)')
