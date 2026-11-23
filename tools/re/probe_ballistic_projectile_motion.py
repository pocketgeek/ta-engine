#!/usr/bin/env python3
"""Probe retail BallisticWeapon launch fields and per-tick substeps.

This calls the native 0x52be80 launcher and 0x52bf90 update under Unicorn.
Only the source muzzle lookup and map collision routine are sinks: the first
supplies a known XYZ origin, and the second admits every substep without an
impact. The probe checks the native shot-record layout, launch-angle wiring,
gravity decrement, update order and the number of integration steps.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP


def put(uc, address, value):
    uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def s32(value):
    return struct.unpack('<i', struct.pack('<I', value & 0xffffffff))[0]


def u16(uc, address):
    return struct.unpack('<H', uc.mem_read(address, 2))[0]


def i32s(uc, address, count):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, 4 * count))


p = Icd()
game, shot, weapon_data, wrapper, source, source_aux, update_this = [
    HEAP + i * 0x30000 for i in range(1, 8)
]
origin = (0x00123456, -0x00034567, 0x002468ac)
active_shot = shot
substep_records = []


def muzzle_lookup(uc, sp):
    args = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert args[0] == source
    assert args[1] == active_shot + 4
    assert args[2] & 3 == (struct.unpack('<I', uc.mem_read(wrapper + 0x1a, 4))[0] & 3)
    assert args[3] == 0xffffffff
    uc.mem_write(args[1], struct.pack('<3i', *origin))
    return 4, 0


def admit_substep(uc, sp):
    shot_ptr = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    assert shot_ptr == active_shot
    substep_records.append((i32s(uc, shot_ptr + 4, 3), i32s(uc, shot_ptr + 0x1c, 3),
                            struct.unpack('<3H', uc.mem_read(shot_ptr + 0x34, 6))))
    return 1, 0


p.hooks.update({0x4dd420: muzzle_lookup, 0x52a4d0: admit_substep})
p.freeze_hooks()

put(p.uc, 0x62d55c, game)
put(p.uc, game + 0x19f44, 0)
put(p.uc, source + 0xb4, source_aux)
put(p.uc, source_aux + 0x8a, 0)
put(p.uc, weapon_data + 0x48, 0)
put(p.uc, weapon_data + 0x4c, 0)
put(p.uc, weapon_data + 0x50, 0)
put(p.uc, weapon_data + 0x54, 0)
put(p.uc, weapon_data + 0x58, 0)
put(p.uc, weapon_data + 0xc8, 0)
put(p.uc, wrapper, weapon_data)
put(p.uc, shot, weapon_data)


def trig(address, angle, scale):
    value, error = p.call(address, (angle & 0xffff, scale))
    assert error is None, error
    return s32(value)


rng = random.Random(0x52be8052bf90)
checked_launches = 0
checked_substeps = 0
binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test'
port_rows = []
native_rows = []
for case in range(512):
    steps = 1 + case % 8
    speed = rng.randrange(1, 0x18000)
    source_heading = rng.randrange(65536)
    yaw_offset = rng.randrange(65536)
    pitch = rng.randrange(0x10000)
    piece = case % 4
    tick = rng.randrange(1, 1 << 30)
    gravity = rng.randrange(1, 20000)
    gravity_adj = struct.unpack('<f', struct.pack('<f', rng.uniform(0.15, 2.75)))[0]

    put(p.uc, game + 0x19f44, tick)
    put(p.uc, game + 0x19ecc, gravity)
    put(p.uc, weapon_data + 0xcc, speed)
    put(p.uc, weapon_data + 0xd0, steps)
    p.uc.mem_write(source + 0x7e, struct.pack('<H', source_heading))
    p.uc.mem_write(wrapper + 0x16, struct.pack('<H', yaw_offset))
    p.uc.mem_write(wrapper + 0x18, struct.pack('<H', pitch))
    p.uc.mem_write(wrapper + 0x1a, struct.pack('<H', piece))
    p.uc.mem_write(update_this + 8, struct.pack('<f', gravity_adj))
    active_shot = shot

    launch_yaw = (source_heading + yaw_offset) & 0xffff
    pitch_cos = trig(0x5360f3, pitch, speed)
    expected_velocity = (
        trig(0x5360bf, launch_yaw, pitch_cos),
        trig(0x5360bf, pitch, speed),
        trig(0x5360f3, launch_yaw, pitch_cos),
    )

    _, error = p.call(0x52be80, (shot, source, wrapper))
    assert error is None, (case, error)
    assert i32s(p.uc, shot + 4, 3) == origin
    assert i32s(p.uc, shot + 0x1c, 3) == expected_velocity
    assert struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6)) == (0, launch_yaw, pitch)
    assert struct.unpack('<I', p.uc.mem_read(shot + 0x74, 4))[0] == tick
    port_rows.append(f'L {origin[0]} {origin[1]} {origin[2]} {source_heading} {yaw_offset} {pitch} {speed}')
    native_rows.append((i32s(p.uc, shot + 4, 3), i32s(p.uc, shot + 0x1c, 3),
                        struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6))))
    checked_launches += 1

    gravity_step = int((gravity * gravity_adj) / (steps * steps))
    expected_position = list(origin)
    expected_velocity_now = list(expected_velocity)
    for _ in range(3):
        before_position = i32s(p.uc, shot + 4, 3)
        before_velocity = i32s(p.uc, shot + 0x1c, 3)
        before_angles = struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6))
        port_rows.append('T ' + ' '.join(map(str, (*before_position, *before_velocity, *before_angles,
            gravity, format(gravity_adj, '.9g'), steps, 0, 0, 0))))
        substep_records.clear()
        expected_records = []
        for _ in range(steps):
            expected_velocity_now[1] = s32(expected_velocity_now[1] - gravity_step)
            expected_position = [s32(pos + vel) for pos, vel in zip(expected_position, expected_velocity_now)]
            expected_records.append((tuple(expected_position), tuple(expected_velocity_now), None))
        _, error = p.call(0x52bf90, (shot,), ecx=update_this)
        assert error is None, (case, error)
        got = substep_records
        assert len(got) == steps, (case, len(got), steps)
        for actual, expected in zip(got, expected_records):
            assert actual[0] == expected[0], (case, actual, expected)
            assert actual[1] == expected[1], (case, actual, expected)
            assert actual[2][0:2] == (0, launch_yaw), (case, actual[2])
        assert i32s(p.uc, shot + 4, 3) == tuple(expected_position)
        assert i32s(p.uc, shot + 0x1c, 3) == tuple(expected_velocity_now)
        native_rows.append((i32s(p.uc, shot + 4, 3), i32s(p.uc, shot + 0x1c, 3),
                            struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6))))
        checked_substeps += steps

ported = subprocess.run([binary, '--ballistic-projectile'], input='\n'.join(port_rows) + '\n',
                         text=True, capture_output=True, check=True)
ported_rows = [tuple(map(int, line.split())) for line in ported.stdout.splitlines()]
assert len(ported_rows) == len(native_rows), (len(ported_rows), len(native_rows))
for index, (actual, expected) in enumerate(zip(ported_rows, native_rows)):
    assert actual[:6] == (*expected[0], *expected[1]), (index, actual, expected)
    # Native x87 asin and TAK's deterministic atan formulation differ by at most
    # one BAM unit on float-boundary cases; all other position and angle fields
    # remain exact.
    assert actual[6:8] == expected[2][:2], (index, actual, expected)
    pitch_delta = ((actual[8] - expected[2][2] + 0x8000) & 0xffff) - 0x8000
    assert abs(pitch_delta) <= 1, (index, actual, expected, pitch_delta)

print(f'PASS: {checked_launches} native launches, {checked_substeps} admitted substeps, and {len(native_rows)} native-vs-TAK launch/tick states match XYZ and roll/yaw exactly (pitch within 1 BAM), with truncating gravity and pre-move velocity update order')
