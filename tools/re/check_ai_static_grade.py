#!/usr/bin/env python3
"""Compare the Classic brain's static-grade plane with retail's.

The constructor planner's reachability test 0x4e1570 runs in mode 1 (the static
grade): a cell is passable unless bit 8 of its navigator-grid nibble is set.
That bit is written in one place, the grid build 0x4dfee0 (0x4e0186 calling
0x4dfe90), which rates every cell with 0x508660, takes the minimum over the
class footprint and marks the cell where that minimum is 0. Per case this runs
0x4dfee0 on a navigator grid of random size and footprint (1 x 1 up to wider
than the map), with 0x508660 answering a random rating 0..7 per cell (sparse
or dense zeros), and reads bit 8 of every cell back.

The port (classic_ai_test --static-grade: ai::classicStaticPlane over the same
ratings) must mark the same cells.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_static_grade.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP

GAME = HEAP                  # +0x19f04 the map's cells
GRID = HEAP + 0x1000         # +4/+6 footprint, +0x340 width, +0x344 height, +0x348 nibbles
SCRATCH = HEAP + 0x2000      # the build's row/column buffer (its malloc)
PLANE = HEAP + 0x3000
CELLS = HEAP + 0x40000       # 14-byte map cells; only their addresses are read here


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=3000)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    case = {}

    def rate(uc_, argv):
        grid, cell = struct.unpack('<II', uc_.mem_read(argv, 8))
        if grid != GRID or (cell - CELLS) % 14: raise AssertionError('0x508660 called with %#x %#x' % (grid, cell))
        return 2, case['rating'][(cell - CELLS) // 14]
    p.hooks[0x508660] = rate
    p.hooks[0x4eb9e0] = lambda uc_, argv: (0, SCRATCH)
    p.hooks[0x4eba00] = lambda uc_, argv: (0, 0)
    p.freeze_hooks()
    put(0x62d55c, GAME)
    put(GAME + 0x19f04, CELLS)

    rng = random.Random(0x4dfee0)
    rows, expected = [], []
    stats = {'cells': 0, 'static': 0, 'edge': 0}
    for n in range(args.cases):
        w, h = rng.randrange(1, 48), rng.randrange(1, 48)
        fx, fz = rng.choice([1, 1, 2, 2, 3, 4, 5, 6, 8]), rng.choice([1, 1, 2, 2, 3, 4, 5, 6, 8])
        density = rng.choice([0.0, 0.005, 0.02, 0.1, 0.4])
        rating = [0 if rng.random() < density else rng.randrange(1, 8) for _ in range(w * h)]
        case['rating'] = rating
        uc.mem_write(GRID, bytes(0x400))
        uc.mem_write(GRID + 4, struct.pack('<hh', fx, fz))
        put(GRID + 0x340, w); put(GRID + 0x344, h); put(GRID + 0x348, PLANE)
        uc.mem_write(PLANE, bytes(4 * w * ((h + 7) // 8)))
        uc.mem_write(SCRATCH, bytes(max(w + fx, h + fz) + 8))
        _, error = p.call(0x4dfee0, (), ecx=GRID)
        if error: raise RuntimeError(error)
        bits = []
        for z in range(h):
            for x in range(w):
                nibbles = struct.unpack('<I', uc.mem_read(PLANE + 4 * ((z >> 3) * w + x), 4))[0]
                bits.append((nibbles >> ((z & 7) * 4)) >> 3 & 1)
        expected.append(bits)
        stats['cells'] += w * h
        stats['static'] += sum(bits)
        stats['edge'] += sum(1 for z in range(h) for x in range(w) if x + fx > w or z + fz > h)
        rows.append(' '.join(map(str, [w, h, fx, fz] + rating)))

    out = subprocess.run([args.binary, '--static-grade'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d grids differ; first: case %d (%s)\n port   %s\n retail %s'
                             % (len(bad), len(rows), i, ' '.join(rows[i].split()[:4]), actual[i], expected[i]))
    print('PASS: %d grid builds, 0 mismatches: %d cells, %d with the static bit (%d of them where the footprint '
          'runs off the map)' % (len(rows), stats['cells'], stats['static'], stats['edge']))


if __name__ == '__main__':
    main()
