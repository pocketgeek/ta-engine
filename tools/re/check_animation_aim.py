#!/usr/bin/env python3
"""Compare direct/guided weapon aiming angles with the local retail executable."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
unit, weapon, delta = [HEAP + i * 0x10000 for i in range(3)]
rng = random.Random(0x52c4e0)
rows, expected = [], []
for i in range(8192):
    dx, dy, dz = (rng.randrange(-1500 * 65536, 1500 * 65536) for _ in range(3))
    if i < 512:
        dx = (i % 8 - 4) * 65536 + (i % 3 - 1)
        dz = ((i // 8) % 8 - 4) * 65536 + ((i // 3) % 3 - 1)
        dy = (i // 64 - 4) * 65536 + (i % 3 - 1)
    heading = rng.randrange(65536)
    p.uc.mem_write(delta, struct.pack('<3i', dx, dy, dz))
    p.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    _, error = p.call(0x52c4e0, (unit, weapon, delta))
    if error:
        raise RuntimeError(error)
    expected.append(struct.unpack('<2H', p.uc.mem_read(weapon + 0x16, 4)))
    rows.append(f'{dx} {dy} {dz} {heading}')
result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else
                         'build-o2/retail_visual_test', '--direct-aim'],
                        input='\n'.join(rows) + '\n', text=True,
                        capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((rows[i], a, b) for i, (a, b) in
                                enumerate(zip(actual, expected)) if a != b),
                               'length mismatch')
print('PASS: 8192 direct/guided heading and pitch pairs match retail')

# SweetSpot is a bounding-box center, not the queried piece's origin.
model, geometry, vertices, output = [HEAP + i * 0x10000 for i in range(3, 7)]
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
put(unit + 0xc0, model)
put(model + 0x1cc, geometry)
put(model + 0x1f0, vertices)
rows, expected = [], []
for i in range(1024):
    count = i % 17
    points = [tuple(rng.randrange(-10000000, 10000000) for _ in range(3))
              for _ in range(count)]
    if i % 3 == 0:
        points = [tuple(abs(v) for v in point) for point in points]
    put(geometry + 4, count)
    if points:
        p.uc.mem_write(vertices, b''.join(struct.pack('<3i', *point) for point in points))
    _, error = p.call(0x4dd2a0, (unit, output, 0))
    if error:
        raise RuntimeError(error)
    expected.append(struct.unpack('<3i', p.uc.mem_read(output, 12)))
    rows.append(' '.join(map(str, [count, *(v for point in points for v in point)])))
result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else
                         'build-o2/retail_visual_test', '--piece-bounds'],
                        input='\n'.join(rows) + '\n', text=True,
                        capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((rows[i], a, b) for i, (a, b) in
                                enumerate(zip(actual, expected)) if a != b),
                               'length mismatch')
print('PASS: 1024 native SweetSpot bounds centers, including empty/one-sided geometry')
