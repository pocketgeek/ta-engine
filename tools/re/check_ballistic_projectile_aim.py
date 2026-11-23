#!/usr/bin/env python3
"""Compare Aramon/Taros Archer aim fields from 0x52bdf0 with TAK helpers."""
import pathlib
import re
import struct
import subprocess
import sys

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def put(uc, address, value):
    uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def float32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test'
root = pathlib.Path(__file__).resolve().parents[2]
p = Icd()
game, source, wrapper, data, delta, weapon = [HEAP + i * 0x10000 for i in range(1, 7)]
put(p.uc, 0x62d55c, game)
put(p.uc, game + 0x19ecc, 8155)
fixtures = []
for faction, filename in [('Aramon', 'araarch.fbi'), ('Taros', 'tararch.fbi')]:
    text = (root / 'assets/extracted/all/units' / filename).read_text(errors='replace')
    weapon_block = re.search(r'\[WEAPON1\]\s*\{(.*?)\n\}', text, re.S)
    assert weapon_block, filename
    velocity_match = re.search(r'weaponvelocity\s*=\s*([0-9.]+)', weapon_block.group(1), re.I)
    assert velocity_match, filename
    velocity = float32(float(velocity_match.group(1)))
    raw_speed = int(float(velocity) * 2184.5333333333333)
    steps = max(1, (raw_speed + 0xfffff) >> 20)
    speed_per_step = raw_speed // steps
    total_speed = speed_per_step * steps
    put(p.uc, wrapper, data)
    put(p.uc, data + 0xcc, speed_per_step)
    put(p.uc, data + 0xd0, steps)
    put(p.uc, weapon + 4, 0)  # both shipped Archer weapons use the low arc.
    p.uc.mem_write(weapon + 8, struct.pack('<f', 1.0))
    vectors = [(80, 0, 100), (200, 16, 200), (350, 0, 100), (440, -16, 0),
               (200, 64, 50), (300, -64, -300), (0, 0, 100), (0, 32, 0),
               (128, -48, 96), (-256, 80, 192)]
    for index, (x, y, z) in enumerate(vectors):
        heading = (index * 0x1eaf + 0x1234) & 0xffff
        p.uc.mem_write(source + 0x7e, struct.pack('<H', heading))
        p.uc.mem_write(delta, struct.pack('<3i', x * 65536, y * 65536, z * 65536))
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x52bdf0, (source, wrapper, delta), ecx=weapon)
        assert error is None, (faction, vectors[index], error)
        native = struct.unpack('<2H', p.uc.mem_read(wrapper + 0x16, 4))
        fixtures.append((faction, (x, y, z, heading), native, total_speed))

direct_rows, ballistic_rows = [], []
for _, (x, y, z, heading), _, total_speed in fixtures:
    direct_rows.append(f'{x*65536} {y*65536} {z*65536} {heading}')
    speed = float32(total_speed / 65536.0)
    ballistic_rows.append(f'{float32(x):.9g} {float32(y):.9g} {float32(z):.9g} '
                          f'{speed:.9g} 1 0 8155')

direct = subprocess.run([binary, '--direct-aim'], input='\n'.join(direct_rows) + '\n',
                        text=True, capture_output=True, check=True)
ballistic = subprocess.run([binary, '--ballistic-aim'], input='\n'.join(ballistic_rows) + '\n',
                           text=True, capture_output=True, check=True)
direct_values = [tuple(map(int, line.split())) for line in direct.stdout.splitlines()]
ballistic_values = list(map(int, ballistic.stdout.split()))
assert len(direct_values) == len(fixtures) == len(ballistic_values)
for index, (faction, vector, native, _) in enumerate(fixtures):
    heading = direct_values[index][0]
    pitch = ballistic_values[index]
    if pitch == 0x8000:  # native aim stores zero when the ballistic solve is unreachable.
        pitch = 0
    actual = (heading, pitch)
    assert actual == native, (index, faction, vector, actual, native)

print(f'PASS: {len(fixtures)} 0x52bdf0 aim fixtures from Aramon/Taros Archer WEAPON1 stats match TAK relative yaw and ballistic pitch helpers')
