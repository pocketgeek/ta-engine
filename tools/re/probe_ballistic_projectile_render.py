#!/usr/bin/env python3
"""Observe retail BallisticWeapon draw dispatch for authored 3DO shots.

The BallisticWeapon vtable at 0x5f36e0 selects 0x52c100 for drawing. This probe
executes that native path with the visibility, viewport and final model renderer
as controlled sinks. It checks that the 3DO renderer receives the shot's actual
XYZ fixed-point position, authored model pointer, stored XYZ angles and owner.
It does not compare rasterized pixels or implement TAK's ballistic trajectory.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
game, config, shot, weapon, explored = [HEAP + i * 0x30000 for i in range(5)]
events = []
admitted = True
terrain_height = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def terrain(uc, sp):
    pointer = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    assert pointer == shot + 4
    events.append(('terrain', struct.unpack('<3i', uc.mem_read(pointer, 12))))
    return 1, terrain_height


def viewport(uc, sp):
    pointer = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    point = struct.unpack('<3i', uc.mem_read(pointer, 12))
    events.append(('viewport', point))
    return 1, int(admitted)


def model(uc, sp):
    events.append(('model', struct.unpack('<4I', uc.mem_read(sp, 16))))
    return 4, 0


p.hooks.update({0x511170: terrain, 0x48c870: viewport, 0x4ff570: model})
p.freeze_hooks()

put(0x62d55c, game)
put(0x62d558, config)
put(config + 8, config + 0x100)
# Use the explored-map path and make the whole native 128x128 map visible.
p.uc.mem_write(config + 0x115, b'\x01')
p.uc.mem_write(game + 0x306f, b'\x00')
player = game + 0x2404
put(player + 0x88, explored)
put(player + 0x8c, 128)
put(player + 0x90, 128)
p.uc.mem_write(explored, bytes([1]) * (128 * 128))
put(shot, weapon)
# No sprite animations: this probe isolates the ballistic 3DO draw path.
put(weapon + 0x54, 0)
put(weapon + 0x58, 0)

rng = random.Random(0x52c100)
for case in range(4096):
    # Keep the projected map cell interior while varying all fractional words.
    xyz = ((rng.randrange(128, 3000) << 16) | rng.randrange(65536),
           (rng.randrange(0, 500) << 16) | rng.randrange(65536),
           (rng.randrange(600, 3000) << 16) | rng.randrange(65536))
    angles = tuple(rng.randrange(65536) for _ in range(3))
    model_pointer = HEAP + 0x240000 + case * 4
    owner = rng.randrange(8)
    terrain_height = rng.randrange(-128, 384)
    admitted = bool(rng.randrange(2))
    p.uc.mem_write(shot + 4, struct.pack('<3i', *xyz))
    p.uc.mem_write(shot + 0x34, struct.pack('<3H', *angles))
    put(shot + 0x54, owner)
    put(shot + 0x93, model_pointer)
    events.clear()
    _, error = p.call(0x52c100, (shot,), ecx=weapon)
    assert not error, (case, error)
    model_events = [event for event in events if event[0] == 'model']
    expected = [('model', (shot + 4, model_pointer, shot + 0x34, owner))] if admitted else []
    assert model_events == expected, (case, events, expected)
    assert len([event for event in events if event[0] == 'viewport']) == 1

print('PASS: 4096 native ballistic draws pass the full XYZ shot record, authored model, stored XYZ angles and owner to the 3DO renderer after viewport admission')
