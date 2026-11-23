#!/usr/bin/env python3
"""Compare complete named-lightning emitter timelines with native 4f46b0.

Native readiness, line generation, linked-list insertion, decay and diffusion
execute. Only allocator/free, CRT stream and graphics-device calls are replaced.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
effect, head, plane, nodes, device, vtable, sources, pointers = [HEAP + i * 0x10000 for i in range(8)]
seed = calls = allocated = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def signed(value):
    return (value + 0x80000000) % 0x100000000 - 0x80000000


def alloc(uc, sp):
    global allocated
    assert read(sp) == 32
    address = nodes + allocated * 32
    allocated += 1
    assert allocated < 2048
    return 0, address


def crt(uc, sp):
    global seed, calls
    seed = (seed * 214013 + 2531011) & 0xffffffff
    calls += 1
    return 0, (seed >> 16) & 32767


p.hooks.update({0x4eb9e0: alloc, 0x4eba00: lambda uc, sp: (0, 0), 0x5d4444: crt,
                0x5ac2f0: lambda uc, sp: (0, device), 0x5ac370: lambda uc, sp: (0, device),
                0x5ac3a0: lambda uc, sp: (0, 0), HEAP + 0x80000: lambda uc, sp: (1, 0)})
p.freeze_hooks()
put(device, vtable)
put(vtable + 0x84, HEAP + 0x80000)
put(effect + 0x642, plane)
put(effect + 0x64e, head)
rng = random.Random(0x4f482b)
rows, expected = [], []
for case in range(512):
    width, height = rng.randrange(8, 65), rng.randrange(8, 33)
    capacity, count = rng.choice((0, 1, 2, 4, 8, 16)), rng.randrange(7)
    intensity, decay, rise = rng.randrange(1, 10) * 256, -rng.randrange(64, 513), rng.randrange(2)
    steps = 32
    seed = initial_seed = rng.randrange(0x100000000)
    calls = allocated = 0
    put(effect + 4, width)
    put(effect + 8, height)
    put(effect + 0x1c, capacity)
    put(effect + 0x20, intensity)
    put(effect + 0x24, decay)
    p.uc.mem_write(effect + 0x28, bytes([rise]))
    put(effect + 0x2e, pointers if count else 0)
    put(effect + 0x32, pointers + count * 4 if count else 0)
    put(effect + 0x652, 0)
    put(effect + 0x656, 0)
    put(head, head)
    put(head + 4, head)
    p.uc.mem_write(plane, bytes(width * height))
    definitions = []
    for index in range(count):
        address = sources + index * 0x80
        put(pointers + index * 4, address)
        put(address, 0x5f2c2c)
        put(address + 0x3d, effect)
        points = [rng.randrange(1, width - 1) * 256, rng.randrange(1, height - 2) * 256,
                  rng.randrange(1, width - 1) * 256, rng.randrange(1, height - 2) * 256]
        rate, fade = rng.choice((-2, 0, 1, 2, 3, 5)), rng.randrange(2)
        for offset, value in zip((0x24, 0x28, 0x34, 0x38), points):
            put(address + offset, value)
        put(address + 0x2c, rate)
        put(address + 0x30, 0)
        p.uc.mem_write(address + 0x3c, bytes([fade]))
        definitions.extend((*points, rate, fade))
    emissions = [rng.randrange(2) if tick < 24 else 0 for tick in range(steps)]
    rows.append(' '.join(map(str, (width, height, capacity, intensity, decay, rise, count,
                                  steps, initial_seed, *definitions, *emissions))))
    for emit in emissions:
        result, error = p.call(0x4f46b0, (emit,), ecx=effect)
        assert not error, (case, error)
        positions = []
        address = read(head)
        while address != head:
            positions.extend(struct.unpack('<3i', p.uc.mem_read(address + 8, 12)))
            address = read(address)
            assert len(positions) <= capacity * 3
        remaining = read(effect + 0x652)
        assert len(positions) == remaining * 3
        texture_hash = 1469598103934665603
        for pixel in p.uc.mem_read(plane, width * height):
            texture_hash = ((texture_hash ^ pixel) * 1099511628211) & 0xffffffffffffffff
        clocks = [signed(read(sources + i * 0x80 + 0x30)) for i in range(count)]
        expected.append((result & 255, seed, calls, read(effect + 0x656), remaining,
                         texture_hash, *clocks, *positions))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
result = subprocess.run([binary, '--lightning-emitter'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b), 'row count')
print('PASS: 512 native lightning-emitter timelines (16384 updates) match capacity, '
      'round-robin clocks, pause/drain behavior, particle state, texture hashes and CRT consumption')
