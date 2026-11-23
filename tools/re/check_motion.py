#!/usr/bin/env python3
"""Compare compiled movement arithmetic with retail and optional decoded trace.

python3 -B tools/re/check_motion.py build-dbg/retail_motion_test [trace.jsonl]
Reads local retail code for emulation; exports no code or asset bytes.
"""
import argparse
import json
import struct
import subprocess
import random

from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn.x86_const import (UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EAX,
                               UC_X86_REG_FPCW, UC_X86_REG_EBX)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('oracle')
    parser.add_argument('trace', nargs='?')
    parser.add_argument('--rng-trace', help='ordered JSONL from trace_rng.py')
    parser.add_argument('--reload-sequence', help='combined frames/RNG from capture_reload.py --ticks')
    args = parser.parse_args()
    if args.reload_sequence and (args.trace or args.rng_trace):
        parser.error('--reload-sequence replaces trace and --rng-trace')
    sequence = None
    if args.reload_sequence:
        with open(args.reload_sequence) as source:
            sequence = json.load(source)
        assert sequence['status'] == 'captured', 'incomplete reload capture'
        frames = sequence['frames']
        assert len(frames) >= 2, 'reload capture has no trajectory'
        assert all(b['tick'] == a['tick'] + 1 for a, b in zip(frames, frames[1:])), 'tick gap'
        assert all(f['rng_before'] == f['rng_after'] and not f['unreadable_units'] for f in frames)
    icd = Icd()
    cases, expected = [], []
    direction_cases = 0
    direction_rng = random.Random(0x53612a)
    vectors = [(y, x) for y in (-2147483648, -65536, -1, 0, 1, 65536, 2147483647)
               for x in (-2147483648, -65536, -1, 0, 1, 65536, 2147483647)]
    vectors += [(direction_rng.randrange(-2147483648, 2147483648),
                 direction_rng.randrange(-2147483648, 2147483648)) for _ in range(10000)]
    icd.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    for y, x in vectors:
        value, error = icd.call(0x53612a, (y, x))
        assert error is None, error
        cases.append(f'h {y} {x}')
        expected.append(str(value & 65535))
        direction_cases += 1
    # Execute the complete segment aim calculation with real integer helpers.
    # Stack locals contain the two navigator points; stop before angle lookup.
    steering_cases = 0
    generator = random.Random(0x4d9bc8)
    points = [(0, 0, 0, 0, end, 0, mode)
              for mode in (0, 1, 7)
              for end in (0, 65535, 65536, 16*65536-1, 16*65536,
                          80*65536-1, 80*65536, 80*65536+1, 200*65536)]
    points += [tuple(generator.randrange(-1000*65536, 1000*65536) for _ in range(6)) + (mode,)
               for mode in (0, 1, 7) for _ in range(200)]
    for x, z, sx, sz, ex, ez, mode, precision in (
            (*point, precision) for point in points for precision in (0x027f, 0x037f)):
        # Unicorn starts with PC=24, unlike the normal x87/Win32 CRT setup.
        # Check both 53/64-bit precision; do not depend on that default.
        icd.uc.reg_write(UC_X86_REG_FPCW, precision)
        ebp = STACK + STACK_SZ - 0x2000
        icd.uc.mem_write(ebp + 8, struct.pack('<I', HEAP))
        icd.uc.mem_write(HEAP + 0x68, struct.pack('<iii', x, 0, z))
        icd.uc.mem_write(ebp - 0x40, struct.pack('<6i', sx, 0, sz, ex, 0, ez))
        icd.uc.mem_write(ebp - 0xc, struct.pack('<i', 5 if mode == 0 else 1))
        icd.uc.reg_write(UC_X86_REG_EBP, ebp)
        icd.uc.reg_write(UC_X86_REG_ESP, ebp - 0x1000)
        icd.uc.emu_start(0x4d9bc8, 0x4d9cec)
        ox = struct.unpack('<i', icd.uc.mem_read(ebp - 0x34, 4))[0]
        oz = struct.unpack('<i', icd.uc.mem_read(ebp - 0x2c, 4))[0]
        cases.append(f'p {x} {z} {sx} {sz} {ex} {ez} {mode}')
        expected.append(f'{ox} {oz}')
        steering_cases += 1
    # Execute the entire three-point acceleration/braking decision. Only the
    # eventual speed update is excluded; all geometry and arithmetic run retail.
    acceleration_cases = 0
    icd.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    for index in range(1600):
        coords = [generator.randrange(-300*65536, 300*65536) for _ in range(8)]
        x, z, sx, sz, ex, ez, tx, tz = coords
        if index % 4 == 0:
            ex, ez = x + generator.randrange(-80*65536, 80*65536), z
        heading = generator.randrange(65536)
        speed, maximum, accel, brake = [generator.randrange(1, 4*65536) for _ in range(4)]
        rate = generator.choice((0, 1, 2500, 8192, 65535))
        ebp = STACK + STACK_SZ - 0x2000
        mover, unit_type = HEAP + 0x1000, HEAP + 0x2000
        icd.uc.mem_write(HEAP + 8, struct.pack('<I', mover))
        icd.uc.mem_write(HEAP + 0x68, struct.pack('<iii', x, 0, z))
        icd.uc.mem_write(HEAP + 0x7e, struct.pack('<H', heading))
        icd.uc.mem_write(HEAP + 0xb4, struct.pack('<I', unit_type))
        icd.uc.mem_write(HEAP + 0x12b, struct.pack('<i', maximum))
        icd.uc.mem_write(mover + 0x20, struct.pack('<i', speed))
        icd.uc.mem_write(mover + 0x36, struct.pack('<H', 0))
        icd.uc.mem_write(unit_type + 0x166, struct.pack('<ii', brake, accel))
        icd.uc.mem_write(unit_type + 0x18e, struct.pack('<H', rate))
        icd.uc.mem_write(ebp - 8, struct.pack('<I', mover))
        icd.uc.mem_write(ebp - 0x4c, struct.pack('<9i', sx, 0, sz, ex, 0, ez, tx, 0, tz))
        for register, value in ((UC_X86_REG_EBP, ebp), (UC_X86_REG_ESP, ebp-0x1000),
                                (UC_X86_REG_ESI, HEAP), (UC_X86_REG_EBX, HEAP+0x68)):
            icd.uc.reg_write(register, value)
        icd.uc.emu_start(0x4d9d15, 0x4d95f0)
        delta = struct.unpack('<i', icd.uc.mem_read(icd.uc.reg_read(UC_X86_REG_ESP)+8, 4))[0]
        cases.append('g ' + ' '.join(map(str, (x, z, sx, sz, ex, ez, tx, tz,
                                              heading, speed, maximum, accel, brake, rate))))
        expected.append(str(delta))
        acceleration_cases += 1
    # Fresh translation cache: later tests stop inside basic blocks traversed
    # above, and Unicorn's cached blocks can otherwise run past that endpoint.
    icd = Icd()
    icd.uc.reg_write(UC_X86_REG_FPCW, 0)
    captured_steering = 0
    if sequence:
        for event in sequence.get('steering_calls', []):
            route = event.get('route_world')
            if not route:
                continue
            assert event['fctrl'] & 0x300 in (0x200, 0x300), event['fctrl']
            x, _, z = event['position_raw']
            sx, sz = [v * 65536 for v in route[0]]
            ex, ez = [v * 65536 for v in route[min(1, len(route) - 1)]]
            mode = (event['movement_flags'] >> 5) & 7
            cases.append(f'p {x} {z} {sx} {sz} {ex} {ez} {mode}')
            expected.append(f"{event['aim_raw'][0]} {event['aim_raw'][2]}")
            captured_steering += 1
            cases.append(f"h {x-event['aim_raw'][0]} {z-event['aim_raw'][2]}")
            expected.append(str(event['requested_heading']))
    trig_cases = 0
    # Every table bin and both sides of its unusual +32 BAM transition, with
    # positive/negative magnitudes that exercise product rounding.
    for index in range(512):
        for delta in (-33, -32, -31, 0):
            heading = (index * 128 + delta) & 65535
            for magnitude in (106521, -106521, 65536, 1):
                values = []
                for address in (0x5360bf, 0x5360f3):
                    result, error = icd.call(address, (heading, magnitude))
                    assert error is None, error
                    values.append(result if result < 0x80000000 else result - 0x100000000)
                cases.append(f't {heading} {magnitude}')
                expected.append(f'{values[0]} {values[1]}')
                trig_cases += 1
    arc_cases = 0
    # Execute the complete turn-distance arithmetic, including retail's 64-bit
    # multiply/divide helper. Both input angles are BAM, not radians.
    for speed in (0, 106521, 327680):
        for angle in (0, 1, 2500, 16384, 32768):
            for rate in (0, 1, 2500, 65535):
                ebp = STACK + STACK_SZ - 0x2000
                icd.uc.mem_write(ebp - 8, struct.pack('<I', HEAP))
                icd.uc.mem_write(ebp - 0x18, struct.pack('<I', angle))
                icd.uc.mem_write(HEAP + 0x20, struct.pack('<i', speed))
                icd.uc.reg_write(UC_X86_REG_EBP, ebp)
                icd.uc.reg_write(UC_X86_REG_ESP, ebp - 0x1000)
                icd.uc.reg_write(UC_X86_REG_ECX, rate)
                icd.uc.emu_start(0x4da53a, 0x4da56e)
                raw = icd.uc.reg_read(UC_X86_REG_EAX)
                raw = raw if raw < 0x80000000 else raw - 0x100000000
                packed = (rate << 16) | angle
                packed = packed if packed < 0x80000000 else packed - 0x100000000
                cases.append(f'a {speed} {packed}')
                expected.append(str(raw))
                arc_cases += 1
    # Execute the two refusal clamp branches through their shared continuation.
    for foot in (1, 2, 3, 4, 15):
        origin = (160 * 65536 - (foot - 1) * 8 * 65536) // (16 * 65536)
        for repeated in (False, True):
            for delta in (-20, -8, -4, 0, 4, 8, 20):
                for epsilon in (-1, 0, 1):
                    proposed = (160 + delta) * 65536 + epsilon
                    ebp = STACK + STACK_SZ - 0x2000
                    icd.uc.mem_write(HEAP + 0x74, struct.pack('<hhhh', origin, origin, foot, foot))
                    for offset in (0x20, 0x18):
                        icd.uc.mem_write(ebp - offset, struct.pack('<i', proposed))
                    icd.uc.reg_write(UC_X86_REG_EBP, ebp)
                    icd.uc.reg_write(UC_X86_REG_ESI, HEAP)
                    icd.uc.reg_write(UC_X86_REG_EDI, proposed)
                    icd.uc.emu_start(0x4db010 if repeated else 0x4db06f, 0x4db0e0)
                    x = struct.unpack('<i', icd.uc.mem_read(ebp - 0x20, 4))[0]
                    z = struct.unpack('<i', icd.uc.mem_read(ebp - 0x18, 4))[0]
                    assert x == z
                    cases.append(f"{'d' if repeated else 'c'} {proposed} {foot}")
                    expected.append(str(x))
    for seed in (0, 1, 1234, 12345, -1, -2147483648):
        _, error = icd.call(0x535d30, (seed,))
        assert error is None, error
        cases.append(f'i {seed} 0')
        expected.append(str(struct.unpack('<I', icd.uc.mem_read(0x64186c, 4))[0]))
    # Execute the full retail string-to-fixed reader (stub only key lookup).
    icd.hooks[0x543110] = lambda uc, address: (1, HEAP)
    for value in ('1.8', '1.21', '0.81', '0.5', '10', '-1.8'):
        icd.uc.mem_write(HEAP, value.encode() + b'\0')
        _, error = icd.call(0x5431f0, (HEAP + 100, HEAP + 200, 0), ecx=HEAP + 300)
        assert error is None, error
        cases.append(f'f {value} 0')
        expected.append(str(struct.unpack('<i', icd.uc.mem_read(HEAP + 100, 4))[0]))
    del icd.hooks[0x543110]
    # Execute the actual x87 scaling/truncation for every possible speed roll.
    for roll in range(201):
        icd.hooks[0x535cc0] = lambda uc, address, r=roll: (1, r)
        multiplier, error = icd.call(0x512430)
        assert error is None, error
        for nominal in (65536, 117964, 117965, 131072):
            cases.append(f's {nominal} {roll}')
            expected.append(str(nominal * multiplier >> 16))
    del icd.hooks[0x535cc0]
    # Check return values AND seed consumption, including unsupported bounds
    # and seeds where modulo-only implementations differ from retail.
    seed = 12345
    for start in (0, 1, 12345, 0x7fffffff, 0xffffffff):
        seed = start
        for bound in (-1, 0, 1, 2, 10, 201, 120) * 100:
            icd.uc.mem_write(0x64186c, struct.pack('<I', seed))
            result, error = icd.call(0x535cc0, (bound,))
            assert error is None, error
            signed = seed if seed < 0x80000000 else seed - 0x100000000
            cases.append(f'r {signed} {bound}')
            seed = struct.unpack('<I', icd.uc.mem_read(0x64186c, 4))[0]
            expected.append(f'{result} {seed}')
    observations = speed_observations = 0
    speed_rolls = {}
    if args.trace or sequence:
        if sequence:
            frames = sequence['frames']
        else:
            with open(args.trace) as source:
                frames = [json.loads(line) for line in source]
        for frame in frames:
            if frame['kind'] != 'frame':
                continue
            for unit in frame['units']:
                observations += 1
                nominal = unit.get('nominal_speed_raw')
                if nominal is not None and nominal > 0:
                    if nominal not in speed_rolls:
                        speed_rolls[nominal] = {
                            nominal * ((900 + r) * 65536 // 1000) >> 16: r
                            for r in range(201)}
                    base = unit['base_speed_raw']
                    assert base in speed_rolls[nominal], ('unexpected individual speed', unit)
                    cases.append(f's {nominal} {speed_rolls[nominal][base]}')
                    expected.append(str(base))
                    speed_observations += 1
                for axis, pos_index in ((0, 0), (1, 2)):
                    cases.append(f"o {unit['position_raw'][pos_index]} {unit['footprint'][axis]}")
                    expected.append(str(unit['cell_origin'][axis]))
    rng_events = []
    if args.rng_trace or sequence:
        if sequence:
            rng_events = sequence['rng_calls']
        else:
            with open(args.rng_trace) as source:
                rng_events = [event for line in source
                              if (event := json.loads(line))['kind'] == 'rng']
        assert rng_events, 'RNG trace contains no calls'
        if sequence:
            assert rng_events[0]['seed_before'] == sequence['frames'][0]['rng_before'], 'initial RNG gap'
        states = []
        for index, event in enumerate(rng_events):
            seed = event['seed_before']
            states.append(seed)
            icd.uc.mem_write(0x64186c, struct.pack('<I', seed))
            result, error = icd.call(0x535cc0, (event['bound'],))
            assert error is None, error
            successor = struct.unpack('<I', icd.uc.mem_read(0x64186c, 4))[0]
            if index + 1 < len(rng_events):
                assert successor == rng_events[index + 1]['seed_before'], ('RNG sequence gap', index)
            signed = seed if seed < 0x80000000 else seed - 0x100000000
            cases.append(f"r {signed} {event['bound']}")
            expected.append(f'{result} {successor}')
        states.append(successor)
        if sequence:
            assert successor == sequence['frames'][-1]['rng_after'], 'final RNG gap'
            cursor = 0
            for frame in sequence['frames']:
                # Every boundary state must occur in order in the captured chain.
                cursor = states.index(frame['rng_before'], cursor)
    proc = subprocess.run([args.oracle, '--oracle'], input='\n'.join(cases) + '\n',
                          text=True, capture_output=True, check=True)
    actual = proc.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected), proc.stderr)
    for case, want, got in zip(cases, expected, actual):
        assert want == got, (case, want, got)
    print(f'PASS: {direction_cases} direction cases, {steering_cases} segment steering cases, {acceleration_cases} acceleration cases, {trig_cases} sine/cosine cases, {arc_cases} turn-distance cases, '
          f'210 clamp cases, 6 numeric conversions, 6 seed initializations, '
          f'201 speed multipliers, 3500 RNG calls and {observations} live unit origins')
    if speed_observations:
        print(f'PASS: {speed_observations} live individual speeds are attainable from recorded nominal speeds')
    if rng_events:
        callers = len({e['return_address'] for e in rng_events})
        print(f'PASS: {len(rng_events)} ordered retail RNG calls from {callers} callers; no seed sequence gaps')
    if sequence:
        print(f"PASS: {len(sequence['frames'])} consecutive frame boundaries covered by RNG chain")
        print(f'PASS: {captured_steering} captured route-point steering calculations')


if __name__ == '__main__':
    main()
