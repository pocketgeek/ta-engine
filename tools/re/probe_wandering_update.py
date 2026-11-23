#!/usr/bin/env python3
"""Observe native wandering phase gates with real animation clocks.

Damage, disposal and active-loop initialization are recorded sinks. Variation
is scheduled in the future, so this isolates phase/motion/damage admission;
it does not claim wander RNG or launch-geometry parity.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
game, weapon, shot, owner, start_art, loop_art, end_art = [HEAP + i * 0x10000 for i in range(7)]
events = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def sink(name, count):
    def call(uc, sp):
        assert read(sp) == shot
        events.append(name)
        return count, 0
    return call


p.hooks.update({0x529af0: sink('dispose', 1), 0x52f6c0: sink('activate', 1),
                0x529c10: sink('damage', 5)})
p.freeze_hooks()
put(0x62d55c, game)
for art, loop in ((start_art, 0), (loop_art, 1), (end_art, 0)):
    p.uc.mem_write(art, struct.pack('<HB', 2, loop))
    p.uc.mem_write(art + 0x2c, struct.pack('<H', 2))
    p.uc.mem_write(art + 0x34, struct.pack('<H', 3))
rng = random.Random(0x52fb10)
for case in range(4096):
    phase = rng.choice(('before', 'activation', 'start', 'loop', 'end'))
    flags = rng.choice((0, 0x1000, 0x1000000, 0x1001000))
    has_start, has_end = rng.randrange(2), rng.randrange(2)
    expires, finishes = rng.randrange(2), rng.randrange(2)
    tick, activation = 100, 99
    if phase == 'before': activation = 101
    if phase == 'activation': activation = 100
    steps = rng.randrange(1, 9)
    position = tuple(rng.getrandbits(32) for _ in range(3))
    velocity = tuple(rng.getrandbits(32) for _ in range(3))
    p.uc.mem_write(shot, bytes(0x100))
    put(shot, weapon);put(shot + 0x7c, owner)
    put(owner + 0x130, flags);put(game + 0x19f44, tick)
    put(weapon + 4, start_art if has_start else 0)
    put(weapon + 8, loop_art);put(weapon + 0xc, end_art if has_end else 0)
    put(weapon + 0xd0, steps)
    put(shot + 0x60, activation);put(shot + 0x64, 1000)
    put(shot + 0x70, tick if expires else tick + 1)
    p.uc.mem_write(shot + 4, struct.pack('<3I', *position))
    p.uc.mem_write(shot + 0x1c, struct.pack('<3I', *velocity))
    current = start_art if phase == 'start' else end_art if phase == 'end' else loop_art
    # Keep start/end art present when explicitly testing their running phase.
    if phase == 'start': put(weapon + 4, start_art)
    if phase == 'end': put(weapon + 0xc, end_art)
    put(shot + 0x44, current)
    p.uc.mem_write(shot + 0x3c, struct.pack('<HHB', 1 if finishes else 0, 1 if finishes else 2,
                                         1 if current == loop_art else 0))
    events.clear()
    _, error = p.call(0x52fb10, (shot,), ecx=weapon)
    assert not error, (case, phase, error)
    moved = False
    expected = []
    if phase == 'before':
        if not flags & 0x1000000 or flags & 0x1000: expected = ['dispose']
    elif phase == 'activation':
        if has_start:
            assert read(shot + 0x44) == start_art
            assert struct.unpack('<HH', p.uc.mem_read(shot + 0x3c, 4)) == (0, 2)
        else: expected = ['activate']
    elif phase == 'start':
        if finishes: expected = ['activate']
    elif phase == 'end':
        if finishes: expected = ['dispose']
        else: moved = True
    else:
        moved = True
        expected = ['damage']
        if expires:
            if has_end: assert read(shot + 0x44) == end_art
            else: expected.append('dispose')
    assert events == expected, (case, phase, events, expected)
    expected_position = tuple((x + steps * v) & 0xffffffff for x, v in zip(position, velocity)) if moved else position
    assert struct.unpack('<3I', p.uc.mem_read(shot + 4, 12)) == expected_position, (case, phase)
print('PASS: 4096 native wandering updates match buildup cancellation, start/loop/end animation gates, wrapped substeps and damage admission')
