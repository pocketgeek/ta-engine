#!/usr/bin/env python3
"""Compare named-effect particle decay and intensity-plane updates with retail.

Runs 4f46b0 with an existing linked particle list and no emission sources.
Only deallocation and graphics-device access/upload are substituted. Native
motion, pruning, diffusion, stamping and the emitter-alive return all execute.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
effect, head, plane, nodes, device, vtable = [HEAP + i * 0x10000 for i in range(6)]
uploads = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def upload(uc, sp):
    uploads.append(read(sp))
    return 1, 0


p.hooks.update({0x4eba00: lambda uc, sp: (0, 0),
                0x5ac2f0: lambda uc, sp: (0, device),
                0x5ac370: lambda uc, sp: (0, device),
                0x5ac3a0: lambda uc, sp: (0, 0), HEAP + 0x60000: upload})
p.freeze_hooks()
put(device, vtable)
put(vtable + 0x84, HEAP + 0x60000)
put(effect + 0x63e, 123)
put(effect + 0x642, plane)
put(effect + 0x64e, head)
rng = random.Random(0x4f46b0)
rows, expected = [], []
for case in range(4096):
    width, height, rise, count = rng.randrange(3, 25), rng.randrange(4, 25), rng.randrange(2), rng.randrange(25)
    pixels = bytes(rng.randrange(256) for _ in range(width * height))
    p.uc.mem_write(plane, pixels)
    put(effect + 4, width)
    put(effect + 8, height)
    p.uc.mem_write(effect + 0x28, bytes([rise]))
    put(effect + 0x652, count)
    put(head, nodes if count else head)
    put(head + 4, nodes + (count - 1) * 32 if count else head)
    inputs = []
    for index in range(count):
        address = nodes + index * 32
        put(address, address + 32 if index + 1 < count else head)
        put(address + 4, address - 32 if index else head)
        if case % 4 == 0:
            values = [rng.randrange(-0x80000000, 0x80000000) for _ in range(6)]
        else:
            values = [rng.randrange(-256, (width + 1) * 256), rng.randrange(-256, (height + 1) * 256),
                      rng.randrange(-256, 1024 * 256), rng.randrange(-512, 513),
                      rng.randrange(-512, 513), rng.randrange(-1024, 1)]
        inputs.extend(values)
        p.uc.mem_write(address + 8, struct.pack('<6i', *values))
    uploads.clear()
    result, error = p.call(0x4f46b0, (case & 1,), ecx=effect)
    assert not error, error
    positions = []
    address = read(head)
    previous = head
    while address != head:
        assert read(address + 4) == previous
        positions.extend(struct.unpack('<3i', p.uc.mem_read(address + 8, 12)))
        previous, address = address, read(address)
        assert len(positions) <= count * 3
    assert read(head + 4) == previous
    remaining = read(effect + 0x652)
    assert len(positions) == remaining * 3
    assert bool(result & 255) == bool(remaining)
    assert uploads == [123]
    rows.append(' '.join(map(str, (width, height, rise, count, *pixels, *inputs))))
    expected.append((remaining, *positions, *p.uc.mem_read(plane, width * height)))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
result = subprocess.run([binary, '--effect-field'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b), 'row count')
print('PASS: 4096 native named-effect fields match wrapped particle motion/pruning, '
      'in-place diffusion, last-particle intensity stamping and empty-emitter lifetime')
