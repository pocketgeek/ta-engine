#!/usr/bin/env python3
"""Compare the flight velocity kernel with the user's original retail binary.

Navigator outputs, heading update and visual banking are isolated from this
check. No captured game bytes are embedded in fixtures or written to the repo.
"""
import argparse
import json
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def check(binary, count=4000):
    icd = Icd()
    icd.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    entity, mover, navigator, vtable, kind, game = [HEAP + n * 4096 for n in range(6)]

    def put(address, *values):
        icd.uc.mem_write(address, struct.pack('<' + 'I' * len(values), *(v & 0xffffffff for v in values)))

    def u32(address):
        return struct.unpack('<I', icd.uc.mem_read(address, 4))[0]

    target = drift = (0, 0, 0)

    def navigation(uc, args):
        put(u32(args), *target)
        put(u32(args + 4), *drift)
        uc.mem_write(u32(args + 8), struct.pack('<H', 0))
        return 3, 0

    put(entity + 8, mover)
    put(entity + 0xb4, kind)
    put(mover, navigator)
    put(navigator, vtable)
    put(vtable + 0x10, 0x401000)
    put(0x62d55c, game)
    icd.hooks[0x401000] = navigation
    # These calls change orientation/visual banking, not velocity.
    icd.hooks[0x4d91b0] = lambda uc, args: (3, 0)
    attitude_inputs = []
    def attitude(uc, args):
        attitude_inputs.append(tuple(struct.unpack('<3i', uc.mem_read(u32(args + 4), 12))))
        return 2, 0
    icd.hooks[0x4da620] = attitude
    icd.freeze_hooks()
    rng = random.Random(0x4da7d0)
    cases, expected = [], []
    for index in range(count):
        velocity = tuple(rng.randrange(-800000, 800001) for _ in range(3))
        position = tuple(rng.randrange(-100000000, 100000001) for _ in range(3))
        target = tuple(p + rng.randrange(-40000000, 40000001) for p in position)
        if index % 8 == 0:
            target = tuple(p + rng.randrange(-500000, 500001) for p in position)
        if index % 17 == 0:
            target = position
        drift = tuple(rng.randrange(-200000, 200001) for _ in range(3))
        speed = rng.randrange(0, 1600001)
        maximum = rng.randrange(1, 800001)
        accel = rng.randrange(0, maximum * 2 + 1)
        lateral = rng.randrange(0, maximum + 1)
        heading = rng.randrange(65536)
        direct = index % 3 == 0
        put(entity + 0x68, *position)
        icd.uc.mem_write(entity + 0x7e, struct.pack('<H', heading))
        put(entity + 0xa4, 1 if direct else 2)
        put(game + 0x19f30, 1)
        put(entity + 0x12b, maximum)
        put(mover + 8, *velocity)
        put(mover + 0x20, speed)
        icd.uc.mem_write(mover + 0x36, struct.pack('<H', 2))
        put(kind + 0x166, lateral, accel)
        attitude_inputs.clear()
        _, error = icd.call(0x4da7d0, (entity,), ecx=mover)
        if error:
            raise RuntimeError(error)
        expected.append(tuple(struct.unpack('<3i', icd.uc.mem_read(mover + 8, 12))))
        delta = tuple(struct.unpack('<i', struct.pack('<I', (after-before)&0xffffffff))[0]
                      for before, after in zip(velocity, expected[-1]))
        assert attitude_inputs == [delta], (index, attitude_inputs, delta)
        values = (*velocity, *position, *target, *drift, speed, maximum, accel, lateral, heading, int(direct))
        cases.append('v ' + ' '.join(map(str, values)))
    result = subprocess.run([binary, '--oracle'], input='\n'.join(cases) + '\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError(f'expected {len(expected)} rows, got {len(actual)}')
    for index, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError(f'case {index}: {cases[index]}: port {got}, retail {want}')
    print(f'PASS: {len(cases)} flight velocity cases and native attitude-input deltas')


def check_navigation(binary, count=4000):
    icd = Icd()
    icd.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    entity, nav, controller, vtable, kind, game, cell = [HEAP + n * 4096 for n in range(7)]

    def put(address, *values):
        icd.uc.mem_write(address, struct.pack('<' + 'I' * len(values), *(v & 0xffffffff for v in values)))

    def u32(address):
        return struct.unpack('<I', icd.uc.mem_read(address, 4))[0]

    def destination(uc, args):
        put(u32(args), *target)
        return 1, 0

    def direction(uc, args):
        if has_heading:
            uc.mem_write(u32(args), struct.pack('<H', controller_heading))
        return 1, int(has_heading)

    put(nav + 4, controller, entity)
    put(controller, vtable)
    put(vtable + 8, 0x401000)
    put(vtable + 0x10, 0x401010)
    put(vtable + 0x20, 0x401020, 0x401030)
    put(entity + 0xb4, kind)
    put(entity + 0xa4, cell)
    put(0x62d55c, game)
    icd.hooks[0x401000] = lambda uc, args: (0, 3 if cruise else 2)
    icd.hooks[0x401010] = lambda uc, args: (1, 0)  # arrival is a separate controller query
    icd.hooks[0x401020] = destination
    icd.hooks[0x401030] = direction
    icd.freeze_hooks()
    rng = random.Random(0x524af0)
    cases, expected = [], []
    for index in range(count):
        position = tuple(rng.randrange(-100000000, 100000001) for _ in range(3))
        extent = (16, 160, 320, 321)[index % 4] * 65536
        target = tuple(p + rng.randrange(-extent, extent + 1) for p in position)
        if index % 7 == 0:
            target = (position[0] + extent + index % 3 - 1, position[1], position[2])
        previous = tuple(t + rng.randrange(-1000000, 1000001) for t in target)
        heading, controller_heading = rng.randrange(65536), rng.randrange(65536)
        cruise, has_heading, sea_relative = (bool(index & (1 << n)) for n in range(3))
        height, sea, ground = rng.randrange(-200, 201), rng.randrange(256), rng.randrange(256)
        cruise_height = (height + (sea if sea_relative else ground)) * 65536
        put(entity + 0x68, *position)
        put(nav + 0xc, *previous)
        icd.uc.mem_write(nav + 0x24, struct.pack('<H', heading))
        icd.uc.mem_write(kind + 0x23a, struct.pack('<h', height))
        put(kind + 0x260, 0x400000 if sea_relative else 0)
        icd.uc.mem_write(game + 0x19ef8, bytes([sea]))
        icd.uc.mem_write(cell + 1, bytes([ground]))
        _, error = icd.call(0x524af0, ecx=nav)
        if error:
            raise RuntimeError(error)
        expected.append((*struct.unpack('<6i', icd.uc.mem_read(nav + 0xc, 24)),
                         struct.unpack('<H', icd.uc.mem_read(nav + 0x24, 2))[0]))
        values = (*position, *previous, *target, heading, cruise_height, int(cruise),
                  int(has_heading), controller_heading)
        cases.append('n ' + ' '.join(map(str, values)))
    result = subprocess.run([binary, '--oracle'], input='\n'.join(cases) + '\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError(f'expected {len(expected)} rows, got {len(actual)}')
    for index, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError(f'navigation {index}: {cases[index]}: port {got}, retail {want}')
    print(f'PASS: {len(cases)} flight navigation cases')


def check_capture(binary, capture_path, identity):
    from emureload import CapturedProcess
    with open(capture_path) as source:
        capture = json.load(source)
    process = CapturedProcess(capture)
    result = process.run_mover(identity)
    if result['error'] or result['missing']:
        raise AssertionError(f"incomplete mover execution: {result['error']}, {result['missing']}")
    entry = next(p for p in result['phases'] if 'flight_inputs' in p)
    inputs = entry['flight_inputs']
    if inputs['movement_mode'] != 2:
        raise ValueError('capture must start with an active flight mover')
    values = (*entry['velocity_raw'], *entry['position_raw'], *inputs['destination_raw'],
              *inputs['destination_velocity_raw'], entry['speed_raw'], inputs['maximum_speed_raw'],
              inputs['acceleration_raw'], inputs['lateral_limit_raw'], entry['heading'],
              int(inputs['direct_control']))
    actual = subprocess.run([binary, '--oracle'], input='v ' + ' '.join(map(str, values)) + '\n',
                            text=True, capture_output=True, check=True)
    velocity = list(map(int, actual.stdout.split()))
    expected = next(p for p in result['phases'] if p['routine'] == '0x4dad30')['velocity_raw']
    if velocity != expected:
        raise AssertionError(f'captured velocity: port {velocity}, retail {expected}')
    # This only certifies the first isolated update. Missions and other units
    # have not been advanced; it must not be reported as a complete replay.
    following = next(f for f in capture['frames'] if f['tick'] == result['tick'])
    observed = next(u for u in following['units'] if u['id'] == identity)
    for field in ('position_raw', 'heading', 'speed_raw'):
        if result['after'][field] != observed[field]:
            raise AssertionError(f'captured original mover {field}: {result["after"][field]} != {observed[field]}')
    print(f'PASS: unit {identity} captured velocity kernel and isolated original mover at tick {result["tick"]}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_motion_test')
    parser.add_argument('--count', type=int, default=4000)
    parser.add_argument('--capture', help='optional full-memory capture for a first-update check')
    parser.add_argument('--unit', type=int, default=821)
    args = parser.parse_args()
    check(args.binary, args.count)
    check_navigation(args.binary, args.count)
    if args.capture:
        check_capture(args.binary, args.capture, args.unit)
