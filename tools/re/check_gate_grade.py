#!/usr/bin/env python3
"""Compare World's raw closed/open gate rectangles with original 5088f0.

Controlled terrain, body allocation, gate flag and stamped yard cells are
initial inputs. Native raw grading is not substituted. This excludes automatic
gate capability, scripts and the opening/closing transition itself.
"""
import argparse
import itertools
import struct
import subprocess

from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_trace_test')
    args = ap.parse_args()
    p = Icd()
    game, grid, cells, pool, kind = [HEAP + n for n in (0, 0x20000, 0x30000, 0x40000, 0x50000)]
    unit = pool + 312

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value))

    put(0x62d55c, game)
    put(game + 0x19e98, 8); put(game + 0x19e9c, 8); put(game + 0x19f04, cells)
    put(game + 0x14e84, pool); put(game + 0x14e88, unit)
    put(unit + 0x130, 0x1000001); put(unit + 0xb4, kind)
    p.uc.mem_write(grid + 8, struct.pack('<4h4B', 20, -10000, 20, -10000, 255, 127, 255, 127))
    yard = 'ocCo..cCo'
    rows, expected = [], []
    for gate, opened, flooded in itertools.product((0, 1), repeat=3):
        put(kind + 0x264, gate << 30)
        p.uc.mem_write(game + 0x19ef8, bytes([32 if flooded else 0]))
        for z in range(8):
            for x in range(8):
                record = bytearray(14)
                struct.pack_into('<H', record, 8, 0xffff)
                if 2 <= x < 5 and 2 <= z < 5:
                    char = yard[(z - 2) * 3 + x - 2]
                    occupied = char != '.' and not (opened and char in 'cC')
                    struct.pack_into('<H', record, 0, int(occupied))
                    record[13] = 32 if gate and char in 'cC' else 0
                p.uc.mem_write(cells + (z * 8 + x) * 14, bytes(record))
        for x, z, width, height in itertools.product(range(1, 6), range(1, 6), range(1, 4), range(1, 4)):
            value, error = p.call(0x5088f0, (grid, x, z, width, height))
            if error:
                raise RuntimeError(error)
            rows.append(f'{gate} {opened} {flooded} {x} {z} {width} {height}')
            expected.append(value)
    result = subprocess.run([args.binary, '--gate-grade'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = list(map(int, result.stdout.split()))
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, want, got in zip(rows, expected, actual):
        assert want == got, (row, 'native', want, 'World', got)
    print(f'PASS: {len(rows)} raw gate rectangles: solid frames, c/C passages, open yards and blocked water depth')


if __name__ == '__main__':
    main()
