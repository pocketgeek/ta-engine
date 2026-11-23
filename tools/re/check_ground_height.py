#!/usr/bin/env python3
"""Compare surface-height kernels with original retail execution.

Synthetic terrain/model inputs exercise height, pitch/roll, support rotation,
shoreline clamps, boundary refusal and hover bobbing with a controlled clock.
No height routines are substituted. World update timing is a separate check.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_motion_test')
    ap.add_argument('--cases', type=int, default=15000)
    args = ap.parse_args()
    if args.cases < 1:
        ap.error('--cases must be positive')
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, cells, unit, kind, model, primitive, indices, vertices = [
        HEAP+i*0x20000 for i in range(8)]

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    put(0x62d55c, game)
    put(game+0x19f04, cells)
    put(unit+8, unit+0x400)
    put(unit+0xb4, kind)
    put(kind+0x2a0, model)
    put(kind+0x176, 65536)
    put(kind+0x17a, 65536)
    put(model+0xc, 0)
    put(model+0x24, vertices)
    put(model+0x28, primitive)
    put(primitive+0xc, indices)
    p.uc.mem_write(indices, struct.pack('<4H', 0, 1, 2, 3))
    clock = 0
    p.hooks[0x53ff20] = lambda uc, a: (0, clock)
    p.freeze_hooks()
    rng = random.Random(0x51b2a0)
    rows, expected = [], []
    counts = [0]*5
    for case in range(args.cases):
        width, height = rng.randint(3, 20), rng.randint(3, 20)
        heights = [rng.randrange(256) for _ in range(width*height)]
        if case % 13 == 0:
            heights = [rng.randrange(256)]*len(heights)
        raw = bytearray(len(heights)*14)
        raw[4::14] = bytes(heights)
        p.uc.mem_write(cells, bytes(raw))
        put(game+0x19e98, width)
        put(game+0x19e9c, height)
        x = rng.randrange(-16*65536, width*16*65536+65536)
        z = rng.randrange(-16*65536, height*16*65536+65536)
        if case % 9 == 0:
            x = rng.choice([0, 65535, -1, (width-1)*16*65536,
                            (width-1)*16*65536-1, 0x7fffffff, -0x80000000])
        old_y = rng.randrange(-256*65536, 512*65536)
        heading = rng.choice([0, 1, 16384, 32768, 49152, 65535]) if case % 3 == 0 else rng.randrange(65536)
        sea, waterline, mode = rng.randrange(256), rng.randrange(256), case % 5
        clock, phase = rng.getrandbits(32), rng.randrange(65536)
        elapsed = rng.choice([0, 1, 29, 30, 59, 60, 61, 0xffffffff])
        base_speed = rng.randrange(2, 8*65536)
        speed = rng.choice([0, base_speed//4, base_speed//2, base_speed, rng.randrange(base_speed*2)])
        road, water = rng.choice([0, 32768, 65536, 81920, 131072]), rng.choice([0, 32768, 65536, 98304])
        terrain_flags = rng.choice([0, 0x800, 0x1000, 0x1800])
        factor = road if terrain_flags & 0x800 else water if terrain_flags & 0x1000 else 65536
        effective_speed = base_speed*factor >> 16
        bank_scale,pitch_scale=rng.randrange(4*65536),rng.randrange(4*65536)
        old_pitch,old_roll=rng.randrange(65536),rng.randrange(65536)
        put(kind+0x176,bank_scale);put(kind+0x17a,pitch_scale)
        p.uc.mem_write(unit+0x80,struct.pack('<H',old_pitch))
        p.uc.mem_write(unit+0x7c,struct.pack('<H',old_roll))
        sx, sz = rng.randrange(1, 40*65536), rng.randrange(1, 40*65536)
        support = [(-sx, -sz), (sx, -sz), (sx, sz), (-sx, sz)]
        if case % 7 == 0:
            support = [(a+rng.randrange(-65536,65536), b+rng.randrange(-65536,65536))
                       for a, b in support]
        for i, (a, b) in enumerate(support):
            p.uc.mem_write(vertices+i*12, struct.pack('<3i', a, 0, b))
        p.uc.mem_write(game+0x19ef8, bytes([sea]))
        p.uc.mem_write(kind+0x248, bytes([waterline]))
        put(kind+0x260, [0x100000, 0x101000, 0x80000, 0, 0x1000][mode])
        put(unit+0x130, 0x1004001)
        put(unit+0x12b, base_speed)
        put(unit+0x400+0x20, speed)
        put(unit+0x400+0x2c, 0)
        p.uc.mem_write(unit+0x400+0x36, struct.pack('<H', terrain_flags))
        put(kind+0x172, road)
        put(kind+0x16e, water)
        put(game+0x19f44, elapsed)
        p.uc.mem_write(unit+0x82, struct.pack('<H', phase))
        p.uc.mem_write(unit+0x68, struct.pack('<3i', x, old_y, z))
        p.uc.mem_write(unit+0x7e, struct.pack('<H', heading))
        sampled, error = p.call(0x511170, (unit+0x68,))
        if error:
            raise AssertionError((case, 'terrain', error))
        sampled = struct.unpack('<i', struct.pack('<I', sampled & 0xffffffff))[0]
        _, error = p.call(0x51b2a0, (unit,))
        if error:
            raise AssertionError((case, 'surface', error))
        y = struct.unpack('<i', p.uc.mem_read(unit+0x6c, 4))[0]
        pitch=struct.unpack('<H',p.uc.mem_read(unit+0x80,2))[0]
        roll=struct.unpack('<H',p.uc.mem_read(unit+0x7c,2))[0]
        expected.append((sampled, y,pitch,roll))
        row = [width, height, *heights, x, z, old_y, heading, sea, waterline, mode]
        row.extend(value for point in support for value in point)
        row.extend([clock, elapsed, phase, speed, effective_speed])
        row.extend([bank_scale,pitch_scale,old_pitch,old_roll])
        rows.append(' '.join(map(str, row)))
        counts[mode] += 1
    result = subprocess.run([args.binary, '--ground-height'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError(('row count', len(expected), len(actual)))
    for i, (want, got) in enumerate(zip(expected, actual)):
        if want != got:
            raise AssertionError((i, 'mode', i % 5, 'expected', want, 'actual', got, rows[i]))
    print(f'PASS: {len(rows)} native surface height/pitch/roll cases; upright/hover/floater/support/bobbing={counts}')


if __name__ == '__main__':
    main()
