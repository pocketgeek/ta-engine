#!/usr/bin/env python3
"""Observe ordinary LOS draw dispatch and projection in the retail executable.

Viewport admission, terrain-height lookup, animation-frame selection and final
draws are substituted. The native visibility gates and draw ordering execute.
This does not establish sprite pixels, blending or model rasterization parity.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
game, weapon, shot, config, settings, explored, visible = [
    HEAP + i * 0x30000 for i in range(7)]
events = []
admitted = True
terrain_height = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def viewport(uc, sp):
    assert read(sp) == shot + 4
    return 1, int(admitted)


def terrain(uc, sp):
    assert read(sp) == shot + 4
    return 1, terrain_height


def frame(uc, sp):
    pointer = read(sp)
    assert pointer in (shot + 0x3c, shot + 0x48)
    return 1, 900 if pointer == shot + 0x3c else 901


def sprite(uc, sp):
    events.append(('sprite', *struct.unpack('<6i', uc.mem_read(sp, 24))))
    return 6, 0


def model(uc, sp):
    assert struct.unpack('<4I', uc.mem_read(sp, 16)) == (shot + 4, 902, shot + 0x34, 903)
    events.append(('model',))
    return 4, 0


p.hooks.update({0x48c870: viewport, 0x511170: terrain, 0x536400: frame,
                0x4fac00: sprite, 0x4ff570: model})
p.freeze_hooks()
put(0x62d55c, game)
put(0x62d558, config)
put(config + 8, settings)
put(shot, weapon)
put(shot + 0x54, 903)
put(game + 0x19ef4, visible)
p.uc.mem_write(game + 0x306f, b'\x03')
player = game + 0x2404 + 3 * 0x110
put(player + 0x88, explored)
put(player + 0x8c, 128)
put(player + 0x90, 128)
rng = random.Random(0x52c8c0)
for case in range(4096):
    p.uc.mem_write(explored, bytes(128 * 128))
    p.uc.mem_write(visible, bytes(128 * 128 * 2))
    x = rng.randrange(-64, 4160)
    y = rng.randrange(-200, 800)
    z = rng.randrange(-64, 4160)
    # Fractional words never change the signed whole-word projection.
    for offset, coordinate in zip((4, 8, 12), (x, y, z)):
        put(shot + offset, (coordinate << 16) | rng.randrange(65536))
    camera_x, camera_z = rng.randrange(-1000, 1000), rng.randrange(-1000, 1000)
    put(game + 0x14ed0, camera_x)
    put(game + 0x14ed4, camera_z)
    terrain_height = rng.randrange(-100, 300)
    now = rng.choice((99, 100, 101))
    put(game + 0x19f44, now)
    put(shot + 0x60, 100)
    admitted = bool(rng.randrange(2))
    explored_mode = rng.randrange(2)
    p.uc.mem_write(settings + 0x15, bytes([explored_mode]))
    seen = bool(rng.randrange(2))
    cell_x, cell_z = x >> 5, (z - (y >> 1)) >> 5
    in_bounds = 0 <= cell_x < 128 and 0 <= cell_z < 128
    if in_bounds:
        index = cell_z * 128 + cell_x
        p.uc.mem_write(explored + index, bytes([int(seen)]))
        # Another player's bit must not make this player's projectile visible.
        p.uc.mem_write(visible + index * 2, struct.pack('<H', (8 if seen else 0) | 2))
    foreground, shadow, has_model = (bool(rng.randrange(2)) for _ in range(3))
    put(weapon + 0x54, int(foreground))
    put(weapon + 0x58, int(shadow))
    put(shot + 0x93, 902 if has_model else 0)
    events.clear()
    _, error = p.call(0x52c8c0, (shot,), ecx=weapon)
    assert not error, error
    expected = []
    if now >= 100 and admitted and in_bounds and seen:
        if shadow:
            expected.append(('sprite', 901, x - camera_x,
                             z - camera_z - (terrain_height >> 1), 1, 0, 0))
        if has_model:
            expected.append(('model',))
        if foreground:
            expected.append(('sprite', 900, x - camera_x,
                             z - (y >> 1) - camera_z, 0, 0, 0))
    assert events == expected, (case, events, expected)
print('PASS: 4096 native straight-shot draws match activation/visibility gates, '
      'signed projection, separate terrain-projected shadows and shadow/model/sprite order')
