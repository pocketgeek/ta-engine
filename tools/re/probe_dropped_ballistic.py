#!/usr/bin/env python3
"""Compare DroppedBallistic::initShot with TAK's launch helper.

Executes the native 0x52c3b0 launch path. Only QueryWeapon's muzzle write is
controlled; base shot initialization (0x530710), height/gravity calculation,
integer tick conversion and launch fields run from KINGDOMS.icd.
"""
import random
import struct
import subprocess
import sys
import math

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


def put(uc, address, value):
    uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def i32s(uc, address, count):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, 4 * count))


def truncdiv(numerator, denominator):
    quotient = abs(numerator) // denominator
    return -quotient if numerator < 0 else quotient


p = Icd()
game, shot, source, wrapper, weapon_data, source_aux = [HEAP + i * 0x20000 for i in range(1, 7)]
muzzle = (0, 0, 0)
collision_rows = []


def query_weapon(uc, sp):
    args = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert args[0] == source and args[1] == shot + 4 and args[2] & 3 == 0 and args[3] == 0xffffffff, args
    uc.mem_write(args[1], struct.pack('<3i', *muzzle))
    return 4, 0


def admit_collision(uc, sp):
    pointer = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    assert pointer == shot, hex(pointer)
    collision_rows.append(i32s(uc, shot + 4, 3))
    return 1, 0


p.hooks.update({0x4dd420: query_weapon, 0x52a4d0: admit_collision})
p.freeze_hooks()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
put(p.uc, 0x62d55c, game)
put(p.uc, game + 0x19f44, 123)
put(p.uc, source + 0xb4, source_aux)
put(p.uc, source_aux + 0x8a, 0)
put(p.uc, wrapper, weapon_data)
put(p.uc, wrapper + 0x1a, 0)
p.uc.mem_write(weapon_data, bytes(0x100))
put(p.uc, weapon_data + 0xd0, 1)  # shipped tarbeak's parsed dropped-shot substeps
p.uc.mem_write(wrapper + 8, struct.pack('<f', 1.0))

rng = random.Random(0x52c3b0)
cases = []
launch_cases = []
for index in range(512):
    # These are 16.16 positions. Use realistic map gravity values and include
    # both exact-square and fractional fall-time ratios.
    gravity_raw = rng.randrange(30, 240) * 65536 // 900
    muzzle = (rng.randrange(256, 2048) << 16,
              rng.randrange(100, 500) << 16,
              rng.randrange(256, 2048) << 16)
    target = (muzzle[0] + rng.choice((-1, 1)) * rng.randrange(64, 700) * 65536,
              rng.randrange(0, min(120, (muzzle[1] >> 16) + 1)) << 16,
              muzzle[2] + rng.randrange(-500, 501) * 65536)
    # Every horizontal X delta is nonzero, allowing an independent tick-count
    # check from the native horizontal velocity.
    put(p.uc, game + 0x19ecc, gravity_raw)
    p.uc.mem_write(shot + 4, bytes(0x100))
    for offset, value in ((0x28, target[0]), (0x2c, target[1]), (0x30, target[2])):
        put(p.uc, shot + offset, value)
    result, error = p.call(0x52c3b0, (shot, source, wrapper), ecx=weapon_data)
    assert error is None, (index, error)
    native_position = i32s(p.uc, shot + 4, 3)
    native_velocity = i32s(p.uc, shot + 0x1c, 3)
    native_angles = struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6))
    dy = abs(target[1] - muzzle[1])
    expected_ticks = max(1, math.isqrt((2 * dy) // gravity_raw))
    expected_velocity = (truncdiv(target[0] - muzzle[0], expected_ticks), 0,
                         truncdiv(target[2] - muzzle[2], expected_ticks))
    assert native_position == muzzle, (index, native_position, muzzle)
    assert native_velocity == expected_velocity, (index, native_velocity, expected_velocity,
        'muzzle', muzzle, 'target', target, 'gravity', gravity_raw, 'ticks', expected_ticks)
    assert native_angles == (0, 0, 0xc000), (index, native_angles)
    # Check that 0x52c3b0's X velocity reflects the tick count independently.
    assert native_velocity[0] == truncdiv(target[0] - muzzle[0], expected_ticks)
    launch_case = (*muzzle, *target, gravity_raw)
    launch_cases.append(launch_case)

    # The DroppedBallistic vtable's update slot points at the ordinary native
    # BallisticWeapon::update (0x52bf90), with one substep for Tarbeak's
    # weaponvelocity=10. Confirm the actual 3D falling tick and collision input.
    put(p.uc, shot, weapon_data)
    collision_rows.clear()
    _, error = p.call(0x52bf90, (shot,), ecx=wrapper)
    assert error is None, (index, error)
    stepped_position = i32s(p.uc, shot + 4, 3)
    stepped_velocity = i32s(p.uc, shot + 0x1c, 3)
    stepped_angles = struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6))
    assert collision_rows == [stepped_position], (index, collision_rows, stepped_position)
    cases.append((*launch_case, *stepped_position, *stepped_velocity, *stepped_angles))

binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test'
input_rows = '\n'.join(' '.join(map(str, row)) for row in launch_cases) + '\n'
ported = subprocess.run([binary, '--dropped-ballistic-projectile'], input=input_rows,
                        text=True, capture_output=True, check=True)
ported_rows = [tuple(map(int, line.split())) for line in ported.stdout.splitlines()]
assert len(ported_rows) == len(launch_cases), (len(ported_rows), len(launch_cases))
for index, (row, actual) in enumerate(zip(launch_cases, ported_rows)):
    sx, sy, sz, tx, ty, tz, gravity = row
    ticks = max(1, math.isqrt((2 * abs(ty - sy)) // gravity))
    expected = (ticks, sx, sy, sz, truncdiv(tx - sx, ticks), 0,
                truncdiv(tz - sz, ticks), 0, 0, 0xc000)
    assert actual == expected, (index, actual, expected)

tick_rows = []
for row, launch in zip(cases, ported_rows):
    _, px, py, pz, vx, vy, vz, roll, yaw, pitch = launch
    gravity = row[6]
    tick_rows.append('T {} {} {} {} {} {} {} {} {} {} 1.0 1 0 0 0'.format(
        px, py, pz, vx, vy, vz, roll, yaw, pitch, gravity))
ported_steps = subprocess.run([binary, '--ballistic-projectile'],
    input='\n'.join(tick_rows) + '\n', text=True, capture_output=True, check=True)
step_rows = [tuple(map(int, line.split())) for line in ported_steps.stdout.splitlines()]
assert len(step_rows) == len(cases), (len(step_rows), len(cases))
for index, (row, actual) in enumerate(zip(cases, step_rows)):
    expected = row[7:]
    assert actual[:6] == expected[:6], (index, actual, expected)
    assert actual[6:8] == expected[6:8], (index, actual, expected)
    pitch_delta = ((actual[8] - expected[8] + 0x8000) & 0xffff) - 0x8000
    assert abs(pitch_delta) <= 1, (index, actual, expected, pitch_delta)

print(f'PASS: 512 native DroppedBallistic launches and first 0x52bf90 updates match TAK for height/gravity fall ticks, muzzle origin, truncating X/Z velocities, falling XYZ state, and angles (pitch within 1 BAM)')
