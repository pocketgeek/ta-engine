#!/usr/bin/env python3
"""Observe remote initialization and wandering initialization's nimbus tail.

Caster animation startup is a recorded sink; the real base initializer runs. The
wandering geometry prefix is outside this probe; its final activation branch
executes directly with the geometry-independent inputs supplied.
"""
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ESI, UC_X86_REG_EDI

p = Icd()
game, weapon, shot, owner, aim, unit_type, art, script = [HEAP + i * 0x10000 for i in range(8)]
events = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def animation(uc, sp):
    events.append(struct.unpack('<3I', uc.mem_read(sp, 12)))
    return 3, 0


p.hooks.update({0x537390: animation, 0x52fb03: lambda uc, sp: (0, 0)})
p.freeze_hooks()
put(0x62d55c, game)
put(owner + 0xb4, unit_type)
put(owner + 0xc0, script)
put(unit_type + 0x8a, art)
rng = random.Random(0x52e890)
for case in range(4096):
    tick = rng.getrandbits(32)
    delay = rng.randrange(-100, 1000)
    flag, available = rng.randrange(2), rng.randrange(2)
    position = tuple(rng.getrandbits(32) for _ in range(3))
    p.uc.mem_write(owner + 0x68, struct.pack('<3I', *position))
    put(weapon + 0xc8, 0x40000 if flag else 0)
    put(weapon + 0x7c, delay)
    put(art + 0xdc, 333 if available else 0)
    put(game + 0x19f44, tick)
    expected = (tick + 1 + (delay if flag and available else 0)) & 0xffffffff
    for remote in (True, False):
        p.uc.mem_write(shot, bytes(0x100))
        put(shot, weapon)
        events.clear()
        if remote:
            _, error = p.call(0x52e890, (shot, owner, aim), ecx=weapon)
            assert struct.unpack('<3I', p.uc.mem_read(shot + 4, 12)) == position, case
            assert read(shot + 0x54) == art + 0xa8, case
            assert read(shot + 0x74) == tick, case
        else:
            p.uc.reg_write(UC_X86_REG_ESI, shot)
            p.uc.reg_write(UC_X86_REG_EDI, owner)
            _, error = p.call(0x52faa0, (), ecx=weapon)
        assert not error, (case, remote, error)
        assert read(shot + 0x60) == expected, (case, remote)
        assert events == ([(script + 0x1a4, 333, 0)] if flag and available else []), (case, remote, events)
print('PASS: 8192 native remote/wandering activation branches match nimbus admission, restart arguments and wrapped delay; remote origin uses caster body')
