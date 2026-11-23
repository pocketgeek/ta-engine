#!/usr/bin/env python3
"""Compare the Classic brain's zone labels with retail's.

Per case it runs retail's navigator-grid zone labelling 0x4e0a20 (from its
entry to 0x4e0fbf, where the labelling ends and the shore points' own labels
begin) on one random grid: random 4-bit grades (the static bit 8 among them;
open, blotchy and walled grids), the ten start records (some empty, some near
or on the map edge), up to ten sacred-site records (some empty) over a random
feature plane -- sacred features of footprint 1..4 with their footprint tails
(0xfffe back references), unsacred ones (sacredsite 0 or NaN), out-of-range
indices and blank cells under a record. Everything is retail's own code: the
label vectors' growth (0x409510 / 0x44bb00 / 0x445a40), the feature lookups
and the reachability walk 0x4e1570 (mode 1, unbounded) under it; only
operator new and delete (0x4eb9e0 / 0x4eba00) are replaced by a bump
allocator. Every 0x4e1570 call is logged with its arguments.

The port (classic_ai_test --zone-labels: ai::classicZoneLabels with
ai::classicSacredFeature for a site's footprint and
sim::retailRectangleReachable for the walk) must give the same zone count,
the same start and site labels and the same walks in the same order.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_zone_labels.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

GAME = HEAP                  # +0x19e98/+0x19e9c map size, +0x19ec0 type count, +0x19edc types, +0x19f04 plane
NAV = HEAP + 0x20000         # the grid: +0x14 zones, +0x18/+0x28 label vectors, +0x340.. size and cells
GRID = HEAP + 0x21000
STARTS, SITES = HEAP + 0x24000, HEAP + 0x25000
TYPES = HEAP + 0x26000
PLANE = HEAP + 0x30000
ARENA = HEAP + 0x100000


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=3000)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    arena = [ARENA]
    log = []

    def new(uc_, argv):
        n = get(argv); at = arena[0]; arena[0] = (at + n + 15) & ~15
        return 0, at
    p.hooks.update({0x4eb9e0: new, 0x4eba00: lambda uc_, argv: (0, 0)})
    p.freeze_hooks()

    def walk(uc_, a, size, d):
        argv = uc_.reg_read(UC_X86_REG_ESP) + 4
        words = struct.unpack('<8h', uc_.mem_read(argv, 16))
        log.append(list(words))
    uc.hook_add(UC_HOOK_CODE, walk, begin=0x4e1570, end=0x4e1570)
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: uc_.emu_stop(), begin=0x4e0fbf, end=0x4e0fbf)

    rng = random.Random(0x4e0a20)
    rows, expected = [], []
    stats = dict(zones=0, multi=0, sites=0, wide=0)
    put(0x62d55c, GAME)
    for n in range(args.cases):
        arena[0] = ARENA; log.clear()
        w, h = rng.choice([(rng.randrange(6, 41), rng.randrange(6, 41)), (rng.randrange(30, 97), rng.randrange(30, 97))])
        style = rng.randrange(4)
        if style == 0:
            grade = [rng.choice([6, 7, 6, 7, 14]) for _ in range(w * h)]
        elif style == 1:
            grade = [rng.randrange(16) for _ in range(w * h)]
        else:
            grade = [6] * (w * h)
            for _ in range(rng.randrange(1, 6)):        # walls, some with a gap or two
                if rng.random() < 0.5:
                    x = rng.randrange(w); gaps = {rng.randrange(h) for _ in range(rng.choice([0, 0, 1, 2]))}
                    for z in range(h):
                        if z not in gaps: grade[z * w + x] = 8
                else:
                    z = rng.randrange(h); gaps = {rng.randrange(w) for _ in range(rng.choice([0, 0, 1, 2]))}
                    for x in range(w):
                        if x not in gaps: grade[z * w + x] = 8
            for _ in range(rng.randrange(0, 6)):
                bx, bz = rng.randrange(w), rng.randrange(h)
                for dz in range(rng.randrange(1, 6)):
                    for dx in range(rng.randrange(1, 6)):
                        if bx + dx < w and bz + dz < h: grade[(bz + dz) * w + bx + dx] = 9
        packed = [0] * (w * ((h + 7) // 8))
        for z in range(h):
            for x in range(w): packed[(z // 8) * w + x] |= grade[z * w + x] << (4 * (z % 8))
        uc.mem_write(NAV, bytes(0x354))
        uc.mem_write(NAV + 0x340, struct.pack('<III', w, h, GRID))
        uc.mem_write(GRID, struct.pack('<%dI' % len(packed), *packed))

        ntypes = rng.randrange(1, 6)
        types = []
        # Past the count the table reads as no sacred site (retail's tail path has
        # no count check, so a stale record there would be read).
        uc.mem_write(TYPES, bytes(320 * 16))
        for t in range(ntypes):
            sacred = rng.choice([1.0, 1.0, 1.0, 0.5, 0.0, float('nan'), -2.0])
            fx, fz = rng.randrange(1, 5), rng.randrange(1, 5)
            bits = struct.unpack('<I', struct.pack('<f', sacred))[0]
            rec = TYPES + t * 320
            uc.mem_write(rec, bytes(320))
            uc.mem_write(rec + 0xb0, struct.pack('<hh', fx, fz)); put(rec + 0x134, bits)
            types.append((bits, fx, fz))
        plane = [(0xffff, 0, 0)] * (w * h)
        def place(x, z, t):
            fx, fz = types[t][1], types[t][2]
            plane[z * w + x] = (t, 0, 0)
            for dz in range(fz):
                for dx in range(fx):
                    if (dx or dz) and x + dx < w and z + dz < h: plane[(z + dz) * w + x + dx] = (0xfffe, dx, dz)
        sites = []
        for k in range(rng.choice([0, 1, 2, 3, 5, 8, 10])):
            x, z = rng.randrange(w), rng.randrange(h)
            what = rng.random()
            if what < 0.75: place(x, z, rng.randrange(ntypes))
            elif what < 0.85: plane[z * w + x] = (rng.choice([ntypes, ntypes + 3, 0xfffd]), 0, 0)
            elif what < 0.92 and x and z: place(x - 1, z - 1, rng.randrange(ntypes))   # a tail under the record
            sites.append((int(rng.random() < 0.92), x, z))
        starts = []
        for k in range(10):
            valid = int(rng.random() < (0.85 if k < 4 else 0.35))
            if rng.random() < 0.15: x, z = rng.choice([0, 1, 2, w - 1]), rng.choice([0, 3, h - 2, h - 1])
            else: x, z = rng.randrange(w), rng.randrange(h)
            starts.append((valid, x, z))
        for k, (f, bx, bz) in enumerate(plane):
            uc.mem_write(PLANE + k * 14, bytes(8) + struct.pack('<HBB', f, bz, bx) + bytes(2))
        for base, recs, ptr in ((STARTS, starts, 0x62dbdc), (SITES, sites, 0x62dbcc)):
            for k, (valid, x, z) in enumerate(recs):
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
            put(ptr, base if recs or rng.random() < 0.5 else 0); put(ptr + 4, (base if recs else get(ptr)) + 101 * len(recs))
        put(GAME + 0x19e98, w); put(GAME + 0x19e9c, h)
        put(GAME + 0x19ec0, ntypes); put(GAME + 0x19edc, TYPES); put(GAME + 0x19f04, PLANE)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x4e0a20, ecx=NAV, allow_early_stop=True)
        if error: raise RuntimeError(error)
        zones = s32(get(NAV + 0x14))
        def vec(at):
            b, e = get(at), get(at + 4)
            return [s32(get(a)) for a in range(b, e, 4)] if b else []
        sl, tl = vec(NAV + 0x1c), vec(NAV + 0x2c)
        expected.append([zones] + sl + tl + [len(log)] + [v for e in log for v in e])
        stats['zones'] += zones; stats['multi'] += zones > 1
        stats['sites'] += any(v > 0 for v in tl); stats['wide'] += any(e[6] > 13 or e[7] > 13 for e in log)
        line = [w, h] + grade + [len(starts)] + [v for s in starts for v in s] + [len(sites)] + [v for s in sites for v in s]
        line += [ntypes] + [v for t in types for v in t] + [v for c in plane for v in c]
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--zone-labels'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d grids differ; first: case %d\n port   %s\n retail %s'
                             % (len(bad), len(rows), i, actual[i][:60], expected[i][:60]))
    print('PASS: %d 0x4e0a20 zone labellings, 0 mismatches: %d zones in all, %d grids with several, %d with a '
          'labelled site, %d with a footprint wider than a start' % (len(rows), stats['zones'], stats['multi'],
                                                                     stats['sites'], stats['wide']))


if __name__ == '__main__':
    main()
