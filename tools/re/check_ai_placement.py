#!/usr/bin/env python3
"""Compare the Classic brain's building placement with retail's.

Per case it runs, as a retail AI player's build manager would, the block-grid
draw 0x4110f0 (the eight draws its constructor makes) from a random seed, then
one site search 0x412f80 with those grids on the stream that follows: the
nearest free deposit 0x411510 for a type in the lodestone draw class
(yardmap[0] & 0x80), else the block-grid scatter 0x411200 -- its 30 attempts,
the tower distance (three draws summed, redrawn while over the radius), the
angle through retail's own scaled sine and cosine 0x5360bf / 0x5360f3, the
snap of the footprint's top-left cell into a random spot of its grid block,
and the scan that refuses a footprint covering a sacred feature. The map's
cell plane and feature types are random: anchors with in- and out-of-range
type indices, footprint tails (0xfffe) pointing back at anchors, the other
markers, and sacredsite values of 0, NaN, negative and positive; the sacred-site
records include empty ones and sites off the map. Retail's own cell lookup
0x50e600 runs. Only the placement test 0x507d10 is logged instead of run: it
refuses any footprint that leaves the map and otherwise answers at random, and
the port is handed the same answers in order.

The port (classic_ai_test --placement: ai::classicDrawBlockGrids, then
ai::classicPlaceSite over classicDepositSite / classicScatterSite with
classicSacredCell, classicSiteTower and classicSacredValue) must leave the same
grids and seeds, make the same draws in the same order, ask the placement test
about the same cells in the same order, and return the same site.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_placement.py --binary build-o2/classic_ai_test
"""
import argparse
import math
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESP

GAME = HEAP                  # +0x19e98 width, +0x19e9c height, +0x19f04 cells,
                             # +0x19ec0 feature types, +0x19edc their table
BM = HEAP + 0x1a000          # build manager: +0x105 land grid, +0x111 water grid
TYPE = HEAP + 0x1b000        # the type placed (676-byte record)
WEAPON = HEAP + 0x1c000
YARD = HEAP + 0x1c100
CENTRE, OUT = HEAP + 0x1c200, HEAP + 0x1c300
SITES = HEAP + 0x1d000       # 101-byte sacred-site records
FTYPES = HEAP + 0x20000      # 320-byte feature types (+0x134 sacredsite)
CELLS = HEAP + 0x30000       # 14-byte map cells (+8 feature, +0xa back rows, +0xb back columns)
MARKERS = [0xfffa, 0xfffb, 0xfffc, 0xfffd, 0xffff]


def f32bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
    getw = lambda a: struct.unpack('<h', uc.mem_read(a, 2))[0]
    draws, asked = [], []
    case = {}

    def placement(uc_, argv):
        t, zero, cell, one, last = struct.unpack('<5I', uc_.mem_read(argv, 20))
        if t != TYPE or zero != 0 or one != 1 or last != 0:
            raise AssertionError('placement test called with %#x %d %d %d' % (t, zero, one, last))
        x, z = struct.unpack('<hh', struct.pack('<I', cell))
        inside = 0 <= x and 0 <= z and x + case['fx'] <= case['w'] and z + case['fz'] <= case['h']
        ok = int(inside and case['rng'].random() < case['accept'])
        asked.append((x, z, ok))
        return 5, ok
    p.hooks[0x507d10] = placement
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: draws.append(
        struct.unpack('<i', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP) + 4, 4))[0]), begin=0x535cc0, end=0x535cc0)

    rng = random.Random(0x411200 ^ 0x411510)
    rows, expected = [], []
    stats = {'deposit': 0, 'scatter': 0, 'tower': 0, 'found': 0, 'failed30': 0, 'water': 0, 'sacredRefusal': 0}
    put(0x62d55c, GAME)
    put(0x62a33c, BM)
    for n in range(args.cases):
        draws.clear(); asked.clear()
        w, h = rng.randrange(24, 72), rng.randrange(24, 72)
        put(GAME + 0x19e98, w); put(GAME + 0x19e9c, h); put(GAME + 0x19f04, CELLS)
        uc.mem_write(CELLS, (bytes(8) + b'\xff\xff' + bytes(4)) * (w * h))
        ntypes = rng.randrange(1, 9)
        sacred = [rng.choice([0.0, 0.0, 1.0, 1.0, 2.5, -1.0, float('nan')]) for _ in range(ntypes)]
        put(GAME + 0x19ec0, ntypes); put(GAME + 0x19edc, FTYPES)
        uc.mem_write(FTYPES, bytes(320 * 16))
        for i, v in enumerate(sacred): uc.mem_write(FTYPES + i * 320 + 0x134, struct.pack('<f', v))
        cells = {}
        density = rng.choice([0.0, 0.02, 0.1, 0.3])
        anchors = []
        for z in range(h):
            for x in range(w):
                if rng.random() >= density: continue
                r = rng.random()
                if r < 0.6:
                    f = rng.randrange(ntypes + 2)          # some past the type count
                    cells[(x, z)] = (f, 0, 0); anchors.append((x, z))
                elif r < 0.9 and anchors:
                    ax, az = rng.choice(anchors)
                    if ax <= x and az <= z and x - ax < 256 and z - az < 256:
                        cells[(x, z)] = (0xfffe, x - ax, z - az)
                else:
                    cells[(x, z)] = (rng.choice(MARKERS), 0, 0)
        for (x, z), (f, bx, bz) in cells.items():
            c = CELLS + (z * w + x) * 14
            uc.mem_write(c + 8, struct.pack('<HBB', f, bz, bx))
        nsites = rng.choice([0, 1, 2, 4, 8, 16])
        sites = []
        for k in range(nsites):
            valid = int(rng.random() < 0.9)
            if anchors and rng.random() < 0.7: x, z = rng.choice(anchors)
            else: x, z = rng.randrange(-4, w + 4), rng.randrange(-4, h + 4)
            uc.mem_write(SITES + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
            sites.append((valid, x, z))
        put(0x62dbcc, SITES); put(0x62dbd0, SITES + nsites * 101)

        fx, fz = rng.choice([1, 2, 2, 3, 4, 4, 5, 6]), rng.choice([1, 2, 2, 3, 4, 4, 5, 6])
        mwd = rng.choice([-10000, -10000, -10000, 0, 5])
        bm = rng.choice([0, 0, 0, 1])
        armed = rng.choice([0, 1, 1])
        rang = rng.choice([0, 300, 999, 1000, 1200, rng.randrange(0, 2000)])
        menu = rng.choice([0, 0, 0, 3, -1])
        yard = rng.choice([-1, -1, 0x8f, 0x0f, 0x80, 0x00])
        uc.mem_write(TYPE, bytes(676)); uc.mem_write(WEAPON, bytes(0x100))
        uc.mem_write(TYPE + 0x126, struct.pack('<hh', fx, fz)); uc.mem_write(TYPE + 0x194, struct.pack('<h', mwd))
        uc.mem_write(TYPE + 0x24a, bytes([bm])); put(TYPE + 0x260, 0x10000 if armed else 0)
        put(TYPE + 0x1aa, WEAPON); put(WEAPON + 0x90, rang); put(TYPE + 0x12e, menu)
        if yard >= 0:
            uc.mem_write(YARD, bytes([yard, 0x8f])); put(TYPE + 0x12a, YARD)
        else:
            put(TYPE + 0x12a, 0)
        cx = rng.randrange(-64, w * 16 + 64) * 65536 + rng.randrange(65536)
        cz = rng.randrange(-64, h * 16 + 64) * 65536 + rng.randrange(65536)
        radius = rng.choice([0, 1, 2, 80, 160, 400, 560, 800, 1120, rng.randrange(1, 2400)])
        put(CENTRE, cx); put(CENTRE + 4, 0); put(CENTRE + 8, cz)
        put(OUT, 0x7eadbeef); put(OUT + 8, 0x7eadbeef)
        accept = rng.choice([0.0, 0.1, 0.5, 0.9, 1.0])
        case.update(w=w, h=h, fx=fx, fz=fz, accept=accept, rng=random.Random(rng.getrandbits(32)))
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        uc.mem_write(BM, bytes(0x130))
        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x4110f0, (), ecx=BM)
        if error: raise RuntimeError(error)
        grid = []
        for base in (0x105, 0x111):
            grid += [getw(BM + base), getw(BM + base + 2), getw(BM + base + 4), getw(BM + base + 6), geti(BM + base + 8)]
        grid_seed = struct.unpack('<I', uc.mem_read(0x64186c, 4))[0]
        draws.clear()
        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = p.call(0x412f80, (0, CENTRE, radius, TYPE, OUT))
        if error: raise RuntimeError(error)
        ok = result & 0xff
        row = grid + [grid_seed]
        row += [1, geti(OUT), geti(OUT + 8)] if ok else [0]
        row += [struct.unpack('<I', uc.mem_read(0x64186c, 4))[0], len(draws), *draws, len(asked)]
        row += [v for a in asked for v in a[:2]]
        expected.append(row)
        lode = yard >= 0 and yard & 0x80
        tower = bm == 0 and armed and rang < 1000 and menu == 0
        stats['deposit' if lode else 'scatter'] += 1
        stats['tower'] += bool(tower and not lode)
        stats['water'] += bool(mwd >= 0 and not lode)
        stats['found'] += bool(ok)
        stats['failed30'] += bool(not lode and not ok and len(asked) == 30)
        stats['sacredRefusal'] += bool(not lode and sum(a[2] for a in asked) > int(ok))
        line = [seed, w, h, ntypes] + [f32bits(v) for v in sacred] + [len(cells)]
        line += [v for (x, z), (f, bx, bz) in cells.items() for v in (x, z, f, bx, bz)]
        line += [nsites] + [v for s in sites for v in s]
        line += [cx, cz, radius, fx, fz, mwd, bm, armed, rang, menu, yard, len(asked)] + [a[2] for a in asked]
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--placement'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d placements differ; first: case %d\n port   %s\n retail %s'
                             % (len(bad), len(rows), i, actual[i], expected[i]))
    print('PASS: %d grid draws + site searches, 0 mismatches: %d deposit searches, %d scatters (%d towers, %d on '
          'the water grid, %d that failed all 30 attempts, %d with a site refused for covering a sacred '
          'feature), %d sites found' % (len(rows), stats['deposit'], stats['scatter'], stats['tower'],
                                         stats['water'], stats['failed30'], stats['sacredRefusal'], stats['found']))


if __name__ == '__main__':
    main()
