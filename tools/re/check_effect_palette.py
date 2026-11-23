#!/usr/bin/env python3
"""Compare native effect palette ramps and ARGB4444 intensity conversion.

4f40f0 runs with GPU palette refresh disabled; 4f4ed0 runs unchanged. No
arithmetic is substituted. Includes overlapping ramps and untouched entries.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
effect, record = HEAP, HEAP + 0x10000
palette = effect + 0x3a
rng = random.Random(0x4f4ed0)
rows, expected = [], []
for case in range(4096):
    colors = [rng.randrange(0x100000000) for _ in range(256)]
    p.uc.mem_write(palette, struct.pack('<256I', *colors))
    intensity = rng.choice((-7, 1, 7, 16, 31, 255, 511)) * 256 + rng.randrange(256)
    p.uc.mem_write(effect + 0x20, struct.pack('<i', intensity))
    count = rng.randrange(1, 7)
    records = []
    for operation in range(count):
        ramp = rng.randrange(2)
        first, last = rng.randrange(256), rng.randrange(256)
        start, end = rng.randrange(0x100000000), rng.randrange(0x100000000)
        values = (ramp, first, last, start, end)
        records.extend(values)
        p.uc.mem_write(record, struct.pack('<5I', *values))
        _, error = p.call(0x4f40f0, (record, 0), ecx=palette)
        assert not error, (case, error)
    _, error = p.call(0x4f4ed0, (), ecx=effect)
    assert not error, (case, error)
    expected.append((*struct.unpack('<256I', p.uc.mem_read(palette, 1024)),
                     *struct.unpack('<256H', p.uc.mem_read(effect + 0x43e, 512))))
    rows.append(' '.join(map(str, (intensity, count, *colors, *records))))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
result = subprocess.run([binary, '--effect-palette'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b), 'row count')
print('PASS: 4096 native effect palettes match integer ramp accumulation, endpoint replacement, '
      'overlapping definitions and all 256 ARGB4444 intensity bins')
