#!/usr/bin/env python3
"""Compare BallisticWeapon::inRange (0x52bc60) with tak::retailBallisticInRange.

52bc60 takes target minus shooter ORIGIN (+0x68/+0x6c/+0x70), scales each delta
and the weapon's +0xd0 * +0xcc speed to float by 1/65536, solves the arc with
52bd10 (0x8000: out of range) and then runs the base test 0x530580 (range at
+0x90, the air flags at +0xc8 left clear). Cases mix random deltas with the
shipped Marine Mortar (cresubm: velocity 520, gravityadjustment 2.8,
lobpreferred) around its 750/800 px range, where the arc is what decides.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
game, shooter, target, data, weapon = [HEAP + i * 0x20000 for i in range(5)]


def put(a, v):
    p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


put(0x62d55c, game)
rng = random.Random(0x52bc60)
rows, expected = [], []
mortar_raw = int(f32(520.0) * 2184.5333333333333)
mortar_steps = max(1, (mortar_raw + 0xfffff) >> 20)
for i in range(6000):
    if i % 2:
        # Marine Mortar from 600 to 900 px, the target 0..120 px above or below.
        per, steps, scale, high, gravity = mortar_raw // mortar_steps, mortar_steps, f32(2.8), 1, 8155
        rng_range = rng.choice([750, 800])
        dist = rng.uniform(600, 900)
        ang = rng.uniform(0, 6.283185307179586)
        import math
        dx = int(dist * math.cos(ang) * 65536)
        dz = int(dist * math.sin(ang) * 65536)
        dy = int(rng.uniform(-120, 120) * 65536)
    else:
        raw = rng.randrange(1, 60 << 16)
        steps = max(1, (raw + 0xfffff) >> 20)
        per = raw // steps
        scale = f32(rng.uniform(.05, 4))
        high = rng.randrange(2)
        gravity = rng.randrange(1, 20000)
        rng_range = rng.choice([0, 2, rng.randrange(2, 2000)])
        dx, dy, dz = [rng.randrange(-1500 << 16, 1500 << 16) for _ in range(3)]
        if i % 62 == 0:
            dx = dz = 0
    sx, sy, sz = [rng.randrange(0, 4000 << 16) for _ in range(3)]
    for base, (x, y, z) in ((shooter, (sx, sy, sz)), (target, (sx + dx, sy + dy, sz + dz))):
        p.uc.mem_write(base + 0x68, struct.pack('<3i', x, y, z))
        put(base + 0x130, 0)
    put(data + 0x90, rng_range)
    put(data + 0xc8, 0)
    put(data + 0xcc, per)
    put(data + 0xd0, steps)
    put(weapon + 4, high)
    p.uc.mem_write(weapon + 8, struct.pack('<f', scale))
    put(game + 0x19ecc, gravity)
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    value, error = p.call(0x52bc60, (shooter, target, data), ecx=weapon)
    if error:
        raise RuntimeError(error)
    expected.append(1 if value & 0xffffffff else 0)
    rows.append(f'{dx} {dy} {dz} {per * steps} {scale!r} {high} {gravity} {rng_range}')

binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test'
result = subprocess.run([binary, '--ballistic-inrange'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
actual = list(map(int, result.stdout.split()))
assert len(actual) == len(expected), (len(actual), len(expected))
bad = [(rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
assert not bad, bad[:5]
mortar = [e for i, e in enumerate(expected) if i % 2]
print(f'PASS: {len(expected)} native 52bc60 in-range answers '
      f'({sum(mortar)} of {len(mortar)} Marine Mortar cases in range)')
