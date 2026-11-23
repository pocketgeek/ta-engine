#!/usr/bin/env python3
"""Compare mobile footprint placement with unmodified retail 507d10.

Exercises terrain limits, footprint edges, feature indirection/sentinels and
entity eligibility. Building-placement mode is outside this oracle's scope.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = parser.parse_args()
    p = Icd()
    game, kind, cells, features, entities = (HEAP + offset for offset in
                                           (0, 0x20000, 0x21000, 0x25000, 0x28000))

    def put(address, *values):
        p.uc.mem_write(address, struct.pack('<'+'I'*len(values),
                                          *(v & 0xffffffff for v in values)))

    put(0x62d55c, game)
    put(game + 0x19f04, cells)
    put(game + 0x19edc, features)
    put(game + 0x19ec0, 8)
    put(game + 0x14e84, entities, entities + 7*312)
    p.uc.mem_write(kind + 0x24a, b'\x01')
    p.freeze_hooks()
    rng = random.Random(0x507d10)
    fixtures, expected = [], []
    for case in range(6000):
        width = height = 8
        fx, fz = rng.randint(1, 3), rng.randint(1, 3)
        x, z = rng.randint(-1, 8), rng.randint(-1, 8)
        sea = rng.randint(0, 255)
        max_depth, min_depth = rng.randint(-30, 255), rng.randint(-255, 30)
        slope, water_slope = rng.randint(0, 255), rng.randint(0, 255)
        self_id, allow_moving = rng.randint(1, 7), rng.randrange(2)
        high = low = sea
        # Most fixtures reach the feature/occupancy/terrain branch rather than
        # failing bounds. The remaining fixtures explicitly exercise edges.
        if case % 5:
            x, z = rng.randrange(width-fx), rng.randrange(height-fz)
            max_depth, min_depth = 255, -255
            slope = water_slope = 255
        flags = [rng.choice((0, 0x20, 0x100, 0x120)) for _ in range(8)]
        units = [(1, rng.randrange(2), rng.randrange(2), i) for i in range(8)]
        for i, (_, live, mover, identity) in enumerate(units):
            p.uc.mem_write(entities + i*312 + 2, struct.pack('<H', identity))
            put(entities + i*312 + 8, 1 if mover else 0)
            put(entities + i*312 + 0x130, 0x1000000 if live else 0)
        data = [[0, 0xffff, high, low, 0, 0] for _ in range(width*height)]
        if 0 <= x < width and 0 <= z < height:
            cx, cz = min(x+rng.randrange(fx), width-1), min(z+rng.randrange(fz), height-1)
            cell = data[cz*width+cx]
            branch = case % 12
            if branch < 3:
                cell[0] = rng.choice((self_id, rng.randint(1, 7), 9))
            elif branch < 6:
                cell[1] = rng.choice((*range(10), 0xfffa, 0xfffb, 0xfffc, 0xfffd, 0xffff))
            elif branch < 8:
                if cx or cz:
                    cell[1] = 0xfffe
                    cell[4], cell[5] = (1, 0) if cx else (0, 1)
                    data[(cz-cell[5])*width+cx-cell[4]][1] = rng.choice((*range(8), 0xfffa, 0xffff))
            else:
                cell[3] = rng.randint(0, 255)
                cell[2] = rng.randint(cell[3], 255)
                max_depth, min_depth = rng.randint(-30, 255), rng.randint(-255, 30)
                slope, water_slope = rng.randint(0, 255), rng.randint(0, 255)
        put(game + 0x19e98, width, height)
        p.uc.mem_write(game + 0x19ef8, bytes((sea,)))
        p.uc.mem_write(kind + 0x126, struct.pack('<hh', fx, fz))
        p.uc.mem_write(kind + 0x192, struct.pack('<hh', max_depth, min_depth))
        p.uc.mem_write(kind + 0x23c, bytes((slope, water_slope)))
        for i, flag in enumerate(flags):
            put(features + i*320 + 0x13c, flag)
        for i, (entity, feature, hi, lo, bx, bz) in enumerate(data):
            raw = bytearray(14)
            struct.pack_into('<H', raw, 0, entity)
            raw[5:7] = bytes((hi, lo))
            struct.pack_into('<H', raw, 8, feature)
            raw[10:12] = bytes((bz, bx))
            p.uc.mem_write(cells + i*14, bytes(raw))
        packed = ((z & 0xffff) << 16) | (x & 0xffff)
        packed = struct.unpack('<i', struct.pack('<I', packed))[0]
        result, error = p.call(0x507d10, (kind, self_id, packed, 1, allow_moving))
        if error:
            raise AssertionError((case, error))
        expected.append(result)
        row = [x,z,fx,fz,width,height,sea,max_depth,min_depth,slope,water_slope,
               self_id,allow_moving,8,8,*flags]
        row.extend(value for unit in units for value in unit)
        row.extend(value for cell in data for value in cell)
        fixtures.append(' '.join(map(str, row)))
    output = subprocess.run([args.binary, '--mobile-placement'],
                            input='\n'.join(fixtures)+'\n', text=True,
                            capture_output=True, check=True)
    actual = list(map(int, output.stdout.split()))
    if len(actual) != len(expected):
        raise AssertionError('oracle row count mismatch')
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError((i, fixtures[i], got, want))
    print(f'PASS: {len(fixtures)} mobile placements ({sum(expected)} accepted), original executable')


if __name__ == '__main__':
    main()
