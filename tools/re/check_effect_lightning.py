#!/usr/bin/env python3
"""Compare native named lightning-source intensity pixels and CRT consumption.

Executes 4f3f20 and its recursive midpoint routine unchanged. The CRT hook
supplies a known stream; no line construction or plane writes are substituted.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
source, effect, plane, output = [HEAP + i * 0x10000 for i in range(4)]
seed = calls = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def crt(uc, sp):
    global seed, calls
    seed = (seed * 214013 + 2531011) & 0xffffffff
    calls += 1
    return 0, (seed >> 16) & 32767


p.hooks[0x5d4444] = crt
p.freeze_hooks()
put(source + 0x3d, effect)
put(effect + 0x642, plane)
rng = random.Random(0x4f3f20)
rows, expected = [], []
for case in range(2048):
    width, height = rng.randrange(4, 257), rng.randrange(4, 65)
    if case < 2:
        width, height = 256, 32
        points = [256, 16 * 256, 252 * 256, 16 * 256]
    else:
        points = [rng.randrange(width * 256), rng.randrange(height * 256),
                  rng.randrange(width * 256), rng.randrange(height * 256)]
    intensity, fade = rng.randrange(1, 256) * 256, rng.randrange(2)
    seed = initial_seed = rng.randrange(0x100000000)
    calls = 0
    p.uc.mem_write(plane, bytes(width * height))
    put(effect + 4, width)
    put(effect + 8, height)
    put(effect + 0x20, intensity)
    for offset, value in zip((0x24, 0x28, 0x34, 0x38), points):
        put(source + offset, value)
    p.uc.mem_write(source + 0x3c, bytes([fade]))
    _, error = p.call(0x4f3f20, (output,), ecx=source)
    assert not error, (case, error)
    assert struct.unpack('<2i', p.uc.mem_read(output, 8)) == tuple(points[:2])
    assert p.uc.mem_read(output + 12, 8) == bytes(8)
    expected.append((seed, calls, *p.uc.mem_read(plane, width * height)))
    rows.append(' '.join(map(str, (width, height, *points, intensity, fade, initial_seed))))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
result = subprocess.run([binary, '--effect-lightning'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((i, a[:2], b[:2]) for i, (a, b) in enumerate(zip(actual, expected)) if a != b), 'row count')
print('PASS: 2048 native named-lightning sources match every intensity texel and CRT draw, '
      'including endpoint reversal, fractional coordinates, fading and midpoint clipping')
