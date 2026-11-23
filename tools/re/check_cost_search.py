#!/usr/bin/env python3
"""Compare every cost-search pop, heap entry and cell flag against retail.

Synthetic grades replace only the terrain query. Goal-controller distance,
trace initialization, cost computation and heap operations execute retail.
This tests the phase-2 kernel, not scheduler delivery or mover behavior.
"""
import argparse
import random
import struct
import subprocess

from emuphase import Phase, OBJ, TYPE
from emu import STACK, STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_EBP, UC_X86_REG_ESP,
                               UC_X86_REG_ESI, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDX)


def digest(values):
    value = 14695981039346656037
    for word in values:
        for byte in struct.pack('<I', word & 0xffffffff):
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def case(index, slice_config=None, boat=False):
    rng = random.Random(0x4142c0 + index)
    width, height = (32, 32) if index < 4 else (48, 40)
    start, goal = (2, 2), (width - 4, height - 4)
    if boat:
        # In the controlled map grade 6 is navigable water and grade 0 is
        # shoreline/land. Exercise an open crossing, a peninsula, and small
        # islands while keeping both endpoints in connected water.
        grades = [6] * (width * height)
        if index % 4 == 1:
            for z in range(height // 2 + 4):
                grades[z*width + width//2] = 0
            for z in range(height // 2 + 4, height // 2 + 9):
                grades[z*width + width//2] = 6
        elif index % 4 == 2:
            for z in range(height):
                for x in range(width):
                    if ((x * 7 + z * 11 + index) % 37) < 5:
                        grades[z*width+x] = 0
            for x in range(2, width-3):
                grades[2*width+x] = 6
            for z in range(2, height-3):
                grades[z*width+width-4] = 6
        elif index % 4 == 3:
            for z in range(5, height-5):
                grades[z*width + width//2] = 0
            for z in range(height//2-2, height//2+3):
                grades[z*width + width//2] = 6
    else:
        grades = [6] * (width * height)
        if index % 4 == 1:
            for z in range(height - 8):
                grades[z*width + width//2] = 0
        elif index % 4 == 2:
            grades = [rng.choice((0, 4, 5, 6, 6, 6, 7)) for _ in grades]
            # Keep one connected path for the reachability initialization.
            for x in range(2, width-3): grades[2*width+x] = 6
            for z in range(2, height-3): grades[z*width+width-4] = 6
        elif index % 4 == 3:
            grades = [rng.choice((4, 5, 6, 7)) for _ in grades]
    grades[start[1]*width+start[0]] = grades[goal[1]*width+goal[0]] = 6
    p = Phase(width, height)
    unit = p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit, start, goal)
    if boat:
        # Match a water-domain floater's native cost profile: water movement
        # flag, floater type bit, and a 4x4 footprint. Terrain grading remains
        # a controlled water/land plane, so this isolates routed cost search.
        mover = struct.unpack('<I', p.uc.mem_read(unit + 8, 4))[0]
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', 4, 4))
        p.uc.mem_write(mover + 0x36, struct.pack('<H', 0x1000))
        p.uc.mem_write(TYPE + 0x260, struct.pack('<I', 0x80000))
        p.uc.mem_write(TYPE + 0x16e, struct.pack('<i', 0x10000))
    if slice_config:
        p.uc.mem_write(OBJ + 0x1ad, struct.pack('<I', slice_config[2]))
    def grade(uc, args):
        x, z = struct.unpack('<ii', uc.mem_read(args, 8))
        return 3, grades[z*width+x] if 0 <= x < width and 0 <= z < height else 0
    p.icd.hooks[0x4139d0] = grade
    p.uc.mem_write(unit + 0x7e, struct.pack('<H', index * 8192 & 65535))
    _, error = p.init()
    assert error is None, error
    p.uc.mem_write(OBJ + 0x165, struct.pack('<I', 10000000))
    p.uc.mem_write(OBJ + 0x5c, struct.pack('<I', 1))
    _, error = p.step()
    assert error is None, error
    assert p.phase() == 2 and p.get(0x14) == 1, ('no phase-2 seed', index, p.phase(), p.get(0x14))
    cell_bytes = bytes(p.uc.mem_read(p.get(0x1c), width*height*4))
    limit = 3000
    lines = [' '.join(map(str, (width, height, start[1]*width+start[0],
             cell_bytes[(start[1]*width+start[0])*4+1], *goal, p.get(0x54), p.get(0x40), limit, p.get(0x44))))]
    if slice_config:
        lines.append(' '.join(map(str, slice_config)))
        p.uc.mem_write(OBJ + 0xec, struct.pack('<I', slice_config[1]))
        # Keep actual scheduler cost-loop instructions; stop before retries and
        # substitute only reconstruction/notification after reaching the goal.
        arrived = [False]
        def reconstruct(uc, args):
            arrived[0] = True
            return 1, 0
        p.icd.hooks[0x414450] = reconstruct
        p.icd.hooks[0x415f10] = lambda uc, args: (0, 0)
        p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: uc.emu_stop(), begin=0x4166f1, end=0x4166f1)
        # Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
        p.icd.allow_early_stop = True
    lines.append(' '.join(str(p.get(0x90 + i*4)) for i in range(8)))
    lines.append(' '.join(str(p.get(0x70 + i*4)) for i in range(8)))
    lines.append(' '.join(str(p.get(i)) for i in (0xc0, 0xc4, 0xbc, 0xc8, 0xb4, 0xb8)))
    lines.extend(f'{g} {cell_bytes[i*4]} {cell_bytes[i*4+1]}' for i, g in enumerate(grades))
    expected = []
    for _ in range(limit):
        prefix = ''
        if slice_config:
            p.uc.mem_write(OBJ + 0x48, struct.pack('<I', 0))
            p.uc.mem_write(OBJ + 0x165, struct.pack('<I', slice_config[0]))
            for register, value in ((UC_X86_REG_EBP, STACK + STACK_SZ - 2048),
                                    (UC_X86_REG_ESP, STACK + STACK_SZ - 4096),
                                    (UC_X86_REG_ESI, OBJ), (UC_X86_REG_EBX, 0),
                                    (UC_X86_REG_EAX, 2),
                                    (UC_X86_REG_ECX, 1), (UC_X86_REG_EDX, slice_config[0])):
                p.uc.reg_write(register, value)
            p.uc.emu_start(0x41665f, 0x416952, timeout=5_000_000)
            eip = p.uc.reg_read(UC_X86_REG_EIP)
            assert eip in (0x4166f1, 0x416952), hex(eip)
            # Arrival is identified by the scheduler's reconstruction hook.
            value = 2 if eip == 0x4166f1 else int(arrived[0])
            prefix = f'{p.get(0x48)} {p.get(0x44)} '
        else:
            value, error = p.step()
            assert error is None, error
        cells = bytes(p.uc.mem_read(p.get(0x1c), width*height*4))
        pointers = struct.unpack('<' + 'I'*p.get(0x14), p.uc.mem_read(p.get(4), p.get(0x14)*4))
        heap = []
        for pointer in pointers:
            _, x, z, cost, priority, entry, run = struct.unpack('<Ihhii hh', p.uc.mem_read(pointer, 20))
            heap.extend((z*width+x, cost, priority, entry, run))
        expected.append(prefix + f'{p.get(0x191)} {value if slice_config else int(bool(value))} {p.get(0x54)} {p.get(0x14)} '
                        f'{digest(cells[i] | cells[i+1] << 8 for i in range(0,len(cells),4))} {digest(heap)}')
        if value or (not slice_config and p.get(0x14) == p.get(0x18)): break
        if not slice_config:
            p.uc.mem_write(OBJ + 0x44, struct.pack('<I', 2))  # scheduler's first-attempt fan
    return '\n'.join(lines) + '\n', expected + ['END']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    parser.add_argument('--cases', type=int, default=16)
    args = parser.parse_args()
    profiles, expected_profiles, world_inputs = [], [], []
    generator = random.Random(0x415170)
    for index in range(240):
        turn = (0, 199, 200, 201, 500, 999, 1000, 2500, 65535)[index % 9]
        foot = generator.randrange(1, 7)
        road = generator.choice((32768, 78643, 81919, 81920, 131072, 196607))
        water = generator.choice((32768, 65536, 90000, 131072))
        flags = generator.choice((0, 0x800, 0x1000, 0x1800))
        floater=index%3!=0
        minimum=generator.choice((-13,-1,0,1,13,255));maximum=generator.choice((0,255))
        heavy = int(floater and minimum>0)
        profiles.append(f'{turn} {foot} {road} {water} {flags} {heavy}')
        world_inputs.append(profiles[-1]+f' {int(floater)} {minimum} {maximum}')
        p = Phase(16, 16)
        unit = p.unit(2, 2)
        assert p.construct() is None
        p.plant_request(unit, (2, 2), (12, 12))
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', foot, foot))
        p.uc.mem_write(TYPE + 0x18e, struct.pack('<H', turn))
        p.uc.mem_write(TYPE + 0x172, struct.pack('<i', road))
        p.uc.mem_write(TYPE + 0x16e, struct.pack('<i', water))
        p.uc.mem_write(TYPE + 0x260, struct.pack('<I', 0x80000 if floater else 0))
        p.uc.mem_write(TYPE + 0x192, struct.pack('<hh', maximum, minimum))
        mover = struct.unpack('<I', p.uc.mem_read(unit + 8, 4))[0]
        p.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
        _, error = p.init()
        assert error is None, error
        expected_profiles.append(' '.join(str(p.get(offset)) for offset in
            (*range(0x70, 0x90, 4), 0xc0, 0xc4, 0xbc, 0xc8, 0xb4, 0xb8)))
    actual_profiles = subprocess.run([args.runner, '--costs'], input='\n'.join(profiles)+'\n',
                                    capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(actual_profiles) == len(expected_profiles)
    for index, (actual, expected) in enumerate(zip(actual_profiles, expected_profiles)):
        assert actual == expected, (profiles[index], actual, expected)
    print(f'PASS: {len(profiles)} retail cost profiles (turn boundaries, footprint, road/water modes and heavy slope)')
    world_profiles=subprocess.run([args.runner,'--world-costs'],input='\n'.join(world_inputs)+'\n',
        capture_output=True,text=True,check=True).stdout.splitlines()
    expected_world=[p+' '+row.split()[5] for p,row in zip(expected_profiles,profiles)]
    assert world_profiles==expected_world, next(((world_inputs[i],a,b) for i,(a,b) in
        enumerate(zip(world_profiles,expected_world)) if a!=b),(len(world_profiles),len(expected_world)))
    print(f'PASS: {len(profiles)} production World cost profiles, including the boat gate')
    total = 0
    for index in range(args.cases):
        data, expected = case(index)
        actual = subprocess.run([args.runner], input=data, capture_output=True, text=True, check=True).stdout.splitlines()
        for n, (a, b) in enumerate(zip(actual, expected)):
            if a != b:
                raise AssertionError(f'case {index}, pop {n+1}: port {a}, retail {b}')
        assert len(actual) == len(expected), (index, len(actual), len(expected))
        total += len(expected) - 1
    print(f'PASS: {args.cases} cost-search fixtures, {total} pops; every heap entry and cell flag/direction matches')
    total = 0
    for index in range(8):
        data, expected = case(index, boat=True)
        actual = subprocess.run([args.runner], input=data, capture_output=True,
                                text=True, check=True).stdout.splitlines()
        for n, (a, b) in enumerate(zip(actual, expected)):
            if a != b:
                raise AssertionError(f'boat case {index}, pop {n+1}: port {a}, retail {b}')
        assert len(actual) == len(expected), (index, len(actual), len(expected))
        total += len(expected) - 1
    print(f'PASS: 8 boat cost-search fixtures, {total} pops; water-domain heap and cell states match retail')
    total = 0
    configs = ((1, 10000, 0), (37, 10000, 0), (503, 10, 0),
               (12000, 1, 0), (37, 10000, 1), (503, 10000, 3), (37, 0, 0))
    for index in range(4):
        for config in configs:
            data, expected = case(index, config)
            actual = subprocess.run([args.runner, '--slices'], input=data, capture_output=True,
                                    text=True, check=True).stdout.splitlines()
            assert actual == expected, (index, config, next(
                ((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                (len(actual), len(expected))))
            total += len(expected) - 1
    print(f'PASS: {4 * len(configs)} cost-slice fixtures, {total} boundaries; work, fan, node-cap checks and heaps match')


if __name__ == '__main__':
    main()
