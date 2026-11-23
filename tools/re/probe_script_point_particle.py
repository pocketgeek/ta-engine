#!/usr/bin/env python3
"""Observe native particles used by script effects 2..5.

Executes construction and updates; only terrain-height sampling is replaced.
Palette values are read from the user's binary, never embedded here.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p = Icd()
particle, origin, velocity, game = [HEAP + i * 0x10000 for i in range(4)]
height = 0
samples = []
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def get(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
def terrain(uc, sp):
    address = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    samples.append(struct.unpack('<3I', uc.mem_read(address, 12)))
    return 1, height
p.hooks[0x511170] = terrain
p.freeze_hooks()
put(0x62d55c, game)
p.uc.mem_write(game + 0x19ef8, bytes((64,)))
colors = struct.unpack('<6I', p.uc.mem_read(0x5f2b3c, 24))
rng = random.Random(0x4f1890)
steps = 0
inputs = []
outputs = []
for case in range(512):
    position = [rng.getrandbits(32) for _ in range(3)]
    motion = [rng.getrandbits(32) for _ in range(3)]
    p.uc.mem_write(origin, struct.pack('<3I', *position))
    p.uc.mem_write(velocity, struct.pack('<3I', *motion))
    period = (8, 16)[case % 2]
    _, error = p.call(0x4f1960, (origin, velocity, 0, period), ecx=particle)
    assert not error, error
    assert get(particle + 0xc) == colors[0]
    frame, countdown = 0, period
    for age in range(period * 6):
        height = 64 if case % 3 == 0 and age == 3 else 63
        def signed(v): return (v + 0x80000000) % 0x100000000 - 0x80000000
        inputs.append(' '.join(map(str, [*[signed(v) for v in position],
            *[signed(v) for v in motion], frame, period, countdown, height, 64])))
        samples.clear()
        value, error = p.call(0x4f1890, (), ecx=particle)
        assert not error, error
        outputs.append(' '.join(map(str, [*struct.unpack('<3i',p.uc.mem_read(particle+0x10,12)),
            get(particle+0x28),get(particle+0x30),int(bool(value & 255))])))
        position = [(a + b) & 0xffffffff for a, b in zip(position, motion)]
        assert samples == [tuple(position)]
        if height >= 64:
            assert not value & 255
            assert get(particle + 0x30) == countdown
            break
        countdown -= 1
        if countdown == 0:
            countdown = period
            frame += 1
        assert get(particle + 0x28) == frame
        assert get(particle + 0x30) == countdown
        assert bool(value & 255) == (frame < 6)
        if frame < 6:
            assert get(particle + 0xc) == colors[frame]
        steps += 1
print(f'PASS: {steps} native script point-particle updates match motion, six color stages, 8/16-tick cadence and terrain-at-water-level termination')

if len(sys.argv) > 1:
    result = subprocess.run([sys.argv[1], '--point-step'], input='\n'.join(inputs)+'\n',
                            text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(outputs), (len(actual),len(outputs))
    for i,(a,b) in enumerate(zip(actual,outputs)):
        assert a == b,(i,inputs[i],a,b)
    print(f'PASS: {len(outputs)} compiled C++ point updates match native outputs, including terrain termination')
