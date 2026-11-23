#!/usr/bin/env python3
"""Compare the Classic brain's threat test and enemy query with retail's.

Per case it runs retail's base threat test 0x412000(side, centre, radius) and
then its enemy query 0x411ae0(side, centre, radius, 0, out) over a random
visible-enemy list (build manager +5: begin +9, end +0xd) of units with random
+0x130 flags (live, dying and other bits) and 16.16 positions, some far enough
off for the squared distances to wrap, with radii from 0 to past 46341 (where
r*r wraps too), the targeting-upgrade flag (+0x79) and its second list (+0x25)
set at random. Both routines run as retail's own code, the vector growth
0x409510 / 0x44b6d0 / 0x413900 / 0x532c00 included; only operator new and
delete (0x4eb9e0 / 0x4eba00) are replaced by a bump allocator.

The port (classic_ai_test --threat: ai::classicThreat and
ai::classicEnemiesNear) must give the same answer and the same units in the
same order.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_threat.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP

BM = HEAP                    # build manager: +9/+0xd list, +0x29/+0x2d upgraded list, +0x79
LIST, LIST2 = HEAP + 0x1000, HEAP + 0x3000
UNITS = HEAP + 0x10000
OUT = HEAP + 0x5000          # the out vector object (allocator byte, begin, end, capacity)
CENTRE = HEAP + 0x5100
ARENA = HEAP + 0x100000
USTRIDE = 0x138


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    arena = [ARENA]

    def new(uc_, argv):
        n = struct.unpack('<I', uc_.mem_read(argv, 4))[0]
        at = arena[0]; arena[0] += (n + 15) & ~15
        return 0, at
    def delete(uc_, argv):
        return 0, 0
    p.hooks.update({0x4eb9e0: new, 0x4eba00: delete})
    p.freeze_hooks()

    rng = random.Random(0x412000)
    rows, expected = [], []
    stats = dict(threat=0, listed=0, upgraded=0, wrapped=0)
    for n in range(args.cases):
        arena[0] = ARENA
        uc.mem_write(BM, bytes(0x100)); uc.mem_write(OUT, bytes(16))
        side = rng.randrange(10)
        for s in range(10): put(0x62a33c + 4 * s, BM if s == side else 0)
        scale = rng.choice([1 << 16, 1 << 20, 1 << 24, 1 << 30])
        cx, cz = rng.randrange(-scale, scale), rng.randrange(-scale, scale)
        uc.mem_write(CENTRE, struct.pack('<iii', cx, rng.randrange(-1 << 20, 1 << 20), cz))
        radius = rng.choice([0, 1, 50, 320, 560, 1120, 4000, 46340, 46341, 70000, rng.randrange(0, 1 << 16)])
        units = []
        def unit(i):
            u = UNITS + i * USTRIDE
            flags = rng.choice([0x1000000, 0x1000000, 0x1000000, 0x1001000, 0, 0x1002000, 0x1000100]) | rng.randrange(4)
            if rng.random() < 0.5:
                x, z = cx + rng.randrange(-radius * 2 - 8, radius * 2 + 8) * 65536, cz + rng.randrange(-radius * 2 - 8, radius * 2 + 8) * 65536
            else:
                x, z = rng.randrange(-1 << 31, 1 << 31), rng.randrange(-1 << 31, 1 << 31)
            x = (x + (1 << 31)) % (1 << 32) - (1 << 31); z = (z + (1 << 31)) % (1 << 32) - (1 << 31)
            put(u + 0x130, flags); uc.mem_write(u + 0x68, struct.pack('<iii', x, 0, z))
            units.append((flags, x, z))
            return u
        k1 = rng.choice([0, 1, 2, 5, 12])
        idx1 = [len(units) + k for k in range(k1)]
        for k in range(k1): put(LIST + 4 * k, unit(len(units)))
        first = LIST if k1 or rng.random() < 0.5 else 0      # an empty list may have no storage
        put(BM + 9, first); put(BM + 0xd, first + 4 * k1)
        upgrade = int(rng.random() < 0.4)
        put(BM + 0x79, upgrade)
        k2 = rng.choice([0, 1, 3, 6])
        idx2 = [len(units) + k for k in range(k2)]
        for k in range(k2): put(LIST2 + 4 * k, unit(len(units)))
        put(BM + 0x29, LIST2); put(BM + 0x2d, LIST2 + 4 * k2)

        threat, error = p.call(0x412000, (side, CENTRE, radius))
        if error: raise RuntimeError(error)
        _, error = p.call(0x411ae0, (side, CENTRE, radius, 0, OUT))
        if error: raise RuntimeError(error)
        begin, end = get(OUT + 4), get(OUT + 8)
        listed = [get(a) for a in range(begin, end, 4)] if begin else []
        ids = [(a - UNITS) // USTRIDE for a in listed]
        expected.append([threat & 0xff, len(ids)] + ids)
        stats['threat'] += threat & 1
        stats['listed'] += len(ids) > 0
        stats['upgraded'] += any(i in idx2 for i in ids)
        stats['wrapped'] += radius * radius >= 1 << 31
        line = [cx, cz, radius, upgrade, len(units)] + [v for u in units for v in u]
        line += [len(idx1)] + idx1 + [len(idx2)] + idx2
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--threat'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d cases differ; first: case %d\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, actual[i], expected[i], rows[i][:2000]))
    print('PASS: %d threat tests and enemy queries, 0 mismatches: %d threats, %d non-empty lists, %d from the '
          'upgrade list, %d with r*r past 2^31' % (len(rows), stats['threat'], stats['listed'], stats['upgraded'],
                                                   stats['wrapped']))


if __name__ == '__main__':
    main()
