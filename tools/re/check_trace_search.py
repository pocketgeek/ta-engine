#!/usr/bin/env python3
"""Compare retail 4146e0 with C++ at every resumable boundary.

Synthetic grade queries replace the world query. Real initialization and
controller-distance instructions execute; no grade is used as goal distance.
"""
import argparse
import random
import struct
import subprocess

from emuphase import Phase, OBJ
from emu import Icd
from check_cost_search import digest
from unicorn.x86_const import UC_X86_REG_EIP


def case(index, budget):
    width, height = 40, 32
    start, goal = (8, 8), (30, 23)
    if index % 8 == 0: goal = (30, 8)
    if index % 8 == 1: start = (1, 1)
    if index % 8 == 2: goal = start
    if index % 8 == 3: start, goal = goal, start
    grades = [6] * (width * height)
    rng = random.Random(0x4146e0 + index)
    if index >= 8:
        if index % 4 == 0:
            for z in range(height - 4): grades[z * width + 20] = 0
        elif index % 4 == 1:
            grades = [rng.choice((0, 0, 4, 5, 6, 6, 7)) for _ in grades]
        elif index % 4 == 2:
            grades = [rng.choice((4, 5, 6, 7)) for _ in grades]
        else:
            for z in range(height): grades[z * width + 20] = 0
    if index != 13: grades[start[1]*width+start[0]] = 6
    else: grades[start[1]*width+start[0]] = 0
    p = Phase(width, height)
    unit = p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit, start, goal)
    assert p.init()[1] is None
    cells = bytes(p.uc.mem_read(p.get(0x1c), width * height * 4))
    lines = [' '.join(map(str, (width, height, *start, *goal, 8, 0, budget, 5000)))]
    lines += [f'{g} {cells[i*4]} {cells[i*4+1]}' for i, g in enumerate(grades)]
    queries = []
    def grade(uc, args):
        x, z, direction = struct.unpack('<iii', uc.mem_read(args, 12))
        queries.extend((x, z, direction))
        return 3, grades[z*width+x] if 0 <= x < width and 0 <= z < height else 0
    p.icd.hooks[0x4139d0] = grade
    p.uc.mem_write(OBJ + 0x165, struct.pack('<I', budget))
    expected = []
    for _ in range(5000):
        p.uc.mem_write(OBJ + 0x48, bytes(4))
        queries.clear()
        result, error = p.icd.call(0x4146e0, ecx=OBJ)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
        if result >= 0x80000000: result -= 0x100000000
        cells = bytes(p.uc.mem_read(p.get(0x1c), width * height * 4))
        values = [result, p.get(0x48)]
        values += [p.get(off) for off in (0x60,0xcc,0xd0,0xd4,0xd8,0xdc,0xe0,0xe4,0xe8,
                                          0xf0,0xf4,0xf8,0xfc,0x100,0x104,0x108,0x10c,0x110,0x4c,0x50)]
        values += list(struct.unpack('<hh', p.uc.mem_read(OBJ + 0x34, 4)))
        values += [digest(cells[i] | cells[i+1] << 8 for i in range(0, len(cells), 4)), digest(queries)]
        expected.append(' '.join(map(str, values)))
        if result != -2: break
    return '\n'.join(lines)+'\n', expected+['END']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    args = parser.parse_args()
    icd = Icd()
    vectors = [(x, z) for x in range(-40, 41) for z in range(-40, 41)]
    expected = []
    for x, z in vectors:
        value, error = icd.call(0x415040, (x, z, -1))
        assert error is None
        expected.append(str(value))
    actual = subprocess.run([args.runner, '--direction'], input=''.join(f'{x} {z}\n' for x, z in vectors),
                            text=True, capture_output=True, check=True).stdout.splitlines()
    assert actual == expected
    print(f'PASS: {len(vectors)} default-hint direction vectors, including 24:10 boundaries')
    total = 0
    for index in range(32):
        for budget in (1, 37, 12000):
            data, expected = case(index, budget)
            actual = subprocess.run([args.runner], input=data, text=True, capture_output=True, check=True).stdout.splitlines()
            assert actual == expected, (index, budget, next(
                ((n, a, b) for n, (a, b) in enumerate(zip(actual, expected)) if a != b),
                (len(actual), len(expected))))
            total += len(expected)-1
    print(f'PASS: 96 reachability fixtures, {total} boundaries; state, cells and ordered grade queries match')


if __name__ == '__main__': main()
