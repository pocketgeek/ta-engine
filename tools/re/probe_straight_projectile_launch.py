#!/usr/bin/env python3
"""Observe native ordinary LOS launch geometry, art selection and lifetime.

QueryWeapon, veterancy and buildup animation startup are controlled. Native
scaled trigonometry executes both in the initializer and as a reference query;
its independent numerical parity is covered by the existing motion probes.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
game, weapon, shot, owner, aim, unit_type, art, script = [
    HEAP + i * 0x10000 for i in range(8)]
origin = (0, 0, 0)
veterancy = slot = 0
events = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def signed(value):
    return (value + 0x80000000) % 0x100000000 - 0x80000000


def muzzle(uc, sp):
    args = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert args[0:2] == (owner, shot + 4) and args[2] & 255 == slot and args[3] == 0xffffffff
    uc.mem_write(shot + 4, struct.pack('<3i', *origin))
    return 4, 0


def veteran(uc, sp):
    assert read(sp) == owner
    return 1, veterancy


def buildup(uc, sp):
    events.append(struct.unpack('<3I', uc.mem_read(sp, 12)))
    return 3, 0


p.hooks.update({0x4dd420: muzzle, 0x519310: veteran, 0x537390: buildup})
p.freeze_hooks()
put(0x62d55c, game)
put(owner + 0xb4, unit_type)
put(owner + 0xc0, script)
put(unit_type + 0x8a, art)
put(aim, weapon)
rng = random.Random(0x52c540)
for case in range(4096):
    p.uc.mem_write(shot, bytes(0xb0))
    put(shot, weapon)
    origin = tuple(rng.randrange(-100000000, 100000000) for _ in range(3))
    pitch = rng.choice((0, 1, 16383, 16384, 16385, 32768, 49152, 65535, rng.randrange(65536)))
    heading, body = rng.randrange(65536), rng.randrange(65536)
    slot = rng.randrange(4)
    p.uc.mem_write(aim + 0x16, struct.pack('<HHB', heading, pitch, slot | 0xf0))
    p.uc.mem_write(owner + 0x7e, struct.pack('<H', body))
    speed, steps, distance = rng.randrange(1, 1048577), rng.randrange(1, 9), rng.randrange(1, 1001)
    put(weapon + 0xcc, speed)
    put(weapon + 0xd0, steps)
    put(weapon + 0x90, distance)
    veterancy, threshold = rng.randrange(10), rng.randrange(10)
    model, upgraded = rng.choice((0, 111)), rng.choice((0, 222))
    put(weapon + 0x48, model)
    put(weapon + 0x4c, upgraded)
    put(weapon + 0x50, threshold)
    flag, has_animation, delay = rng.randrange(2), rng.randrange(2), rng.randrange(100)
    put(weapon + 0xc8, 0x40000 if flag else 0)
    put(art + 0xdc, 333 if has_animation else 0)
    put(weapon + 0x7c, delay)
    tick = rng.randrange(100000)
    put(game + 0x19f44, tick)

    def trig(address, angle, magnitude):
        value, error = p.call(address, (angle, magnitude))
        assert not error, error
        return signed(value)

    horizontal = trig(0x5360f3, pitch, speed)
    velocity = (trig(0x5360bf, (heading + body) & 65535, horizontal),
                signed(-trig(0x5360bf, pitch, speed)),
                trig(0x5360f3, (heading + body) & 65535, horizontal))
    denominator = horizontal or speed
    product = signed(distance * speed)
    quotient = abs(product) // abs(denominator)
    if (product < 0) != (denominator < 0):
        quotient = -quotient
    lifetime = (((quotient << 16) + steps * speed) & 0xffffffff) // (steps * speed)
    start = tick + 1 + (delay if flag and has_animation else 0)
    events.clear()
    _, error = p.call(0x52c540, (shot, owner, aim), ecx=weapon)
    assert not error, (case, error)
    assert struct.unpack('<3i', p.uc.mem_read(shot + 4, 12)) == origin, case
    assert struct.unpack('<3i', p.uc.mem_read(shot + 0x1c, 12)) == velocity, case
    assert struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6)) == (0, heading, (-pitch) & 65535), case
    selected = upgraded if model and upgraded and veterancy >= threshold else model
    assert read(shot + 0x93) == selected, case
    assert read(shot + 0x54) == art + 0xa8, case
    assert (read(shot + 0x60), read(shot + 0x70), read(shot + 0x74)) == (start, start + lifetime, tick), case
    assert events == ([(script + 0x1a4, 333, 0)] if flag and has_animation else []), case
print('PASS: 4096 native straight-shot launches match muzzle, angle/velocity wiring, '
      'veteran art selection, conditional buildup delay and wrapped range-derived lifetime')
