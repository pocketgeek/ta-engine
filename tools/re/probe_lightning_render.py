#!/usr/bin/env python3
"""Native lightning draw endpoint refresh, visibility and fallback dispatch.

This observes 52ccf0 with no named secondary emitter. QueryWeapon, visibility,
viewport and final bolt drawing are controlled; bolt tessellation is not tested.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
game, weapon, shot, owner = [HEAP + i * 0x10000 for i in range(4)]
events = []
muzzle = (0, 0, 0)
slot = 0
viewport_mask = visible_mask = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def args(sp, count):
    return struct.unpack('<' + 'I' * count, p.uc.mem_read(sp, count * 4))


def query(uc, sp):
    values = args(sp, 4)
    assert values[:2] == (owner, shot + 0x10) and values[2] & 255 == slot and values[3] == 0xffffffff
    uc.mem_write(shot + 0x10, struct.pack('<3i', *muzzle))
    events.append('query')
    return 4, 0


def endpoint(pointer):
    assert pointer in (shot + 4, shot + 0x10)
    return 1 if pointer == shot + 0x10 else 2


def viewport(uc, sp):
    return 1, int(bool(viewport_mask & endpoint(args(sp, 1)[0])))


def visible(uc, sp):
    player, pointer = args(sp, 2)
    assert player == game + 0x2404 + 3 * 0x110
    return 2, int(bool(visible_mask & endpoint(pointer)))


def bolt(uc, sp):
    assert args(sp, 3) == (shot + 4, shot + 0x10, weapon + 4)
    events.append('bolt')
    return 3, 0


p.hooks.update({0x4dd420: query, 0x48c870: viewport, 0x531f70: visible, 0x52b400: bolt})
p.freeze_hooks()
put(0x62d55c, game)
p.uc.mem_write(game + 0x306f, b'\x03')
put(shot, weapon)
put(shot + 0x7c, owner)
put(shot + 0x60, 100)
rng = random.Random(0x52ccf0)
for case in range(4096):
    now = rng.choice((99, 100, 101, 150))
    put(game + 0x19f44, now)
    slot = rng.randrange(4)
    put(shot + 0xd8, slot << 2)
    muzzle = tuple(rng.randrange(-0x80000000, 0x80000000) for _ in range(3))
    position = tuple(rng.randrange(-0x80000000, 0x80000000) for _ in range(3))
    p.uc.mem_write(shot + 4, struct.pack('<3i', *position))
    p.uc.mem_write(shot + 0x10, bytes(12))
    viewport_mask, visible_mask = rng.randrange(4), rng.randrange(4)
    events.clear()
    _, error = p.call(0x52ccf0, (shot,), ecx=weapon)
    assert not error, error
    expected = []
    if now >= 100:
        expected.append('query')
        assert struct.unpack('<3i', p.uc.mem_read(shot + 0x10, 12)) == muzzle
        if viewport_mask and visible_mask:
            expected.append('bolt')
    assert events == expected, (case, events, expected)
print('PASS: 4096 native lightning draws refresh the current muzzle, gate either endpoint '
      'independently for viewport/visibility, and connect the moving shot to the current muzzle')
