#!/usr/bin/env python3
"""Compare the Classic brain's strike target picker with retail's.

Per case it runs retail's target picker 0x40a200(centre, member) on a random
game: ten player records (active or not, of kinds 0-4, with an index or the
"none" index 10, allied to the AI or not, with random populations), each owning
a contiguous block of the unit table (live, dying and empty slots, on the ground,
in the air or carried, ready or not (0x2000), cloaked, armed or not, finished or
not (+0x108 0, -0, a fraction or a NaN), mana sites or not), the unit cap, and a
member that is armed or not, with or without a mover, a ground navigator and a
naval movement class, at random positions and ranges.

Retail's own code runs throughout, the octagonal distance 0x535e90 and the game
RNG included. Logged instead of run: the weapon's damage 0x531e50 (it answers
from the case per target), the visibility test 0x4f7210 (likewise) and the
reachability walk 0x4e1570 (its arguments are logged and it answers from the
case's list). The port (classic_ai_test --pick: ai::classicPickTarget, with
retail's player filter applied to the case's records) must make the same draws
and the same damage, visibility and reachability queries in the same order,
pick the same unit and leave the same seed.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_target_pick.py --binary build-o2/classic_ai_test
"""
import argparse
import math
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX

GAME = HEAP                  # +0x2404 player records, +0x14e7c cap, +0x19c70 profiling
MGR = HEAP + 0x22600         # [mgr] the AI's own player record
OWN = HEAP + 0x22800         # its record: +0xac.. the allied table
MEMBER = HEAP + 0x23000
MOVER, NAV = HEAP + 0x23200, HEAP + 0x23300
WEAPON = HEAP + 0x23400
MTYPE = HEAP + 0x23600
UNITS = HEAP + 0x24000
TYPES = HEAP + 0x34000
USTRIDE, TSTRIDE, RSTRIDE = 0x138, 0x2a4, 0x110


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    log = []
    case = {}

    def unit_index(address):
        return (address - UNITS) // USTRIDE

    def damage(uc_, argv):
        if uc_.reg_read(UC_X86_REG_ECX) != WEAPON: raise AssertionError('damage weapon')
        t = unit_index(get(argv))
        log.append((1, t))
        return 1, case['damage'][t]
    def visible(uc_, argv):
        owner, target = struct.unpack('<II', uc_.mem_read(argv, 8))
        if owner != case['owner']: raise AssertionError('visibility owner')
        t = unit_index(target)
        log.append((2, t))
        return 2, case['visible'][t]
    def reach(uc_, argv):
        if uc_.reg_read(UC_X86_REG_ECX) != NAV: raise AssertionError('navigator')
        a = struct.unpack('<6I', uc_.mem_read(argv, 24))
        sx, sz = struct.unpack('<hh', struct.pack('<I', a[0]))
        fx, fz = struct.unpack('<hh', struct.pack('<I', a[1]))
        tx, tz = struct.unpack('<hh', struct.pack('<I', a[2]))
        rx, rz = struct.unpack('<hh', struct.pack('<I', a[3]))
        k = len(case['reached'])
        ok = case['reach'][k] if k < len(case['reach']) else 1
        case['reached'].append(ok)
        log.append((3, sx, sz, fx, fz, tx, tz, rx, rz, a[4], a[5] & 0xff))
        return 6, ok
    p.hooks.update({0x531e50: damage, 0x4f7210: visible, 0x4e1570: reach})
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: log.append(
        (0, s32(get(uc_.reg_read(UC_X86_REG_ESP) + 4)))), begin=0x535cc0, end=0x535cc0)

    rng = random.Random(0x40a200)
    rows, expected = [], []
    picked = scanned = draws = reaches = refused = 0
    put(0x62d55c, GAME)
    for n in range(args.cases):
        log.clear()
        uc.mem_write(GAME, bytes(0x20000))
        uc.mem_write(UNITS, bytes(0x10000)); uc.mem_write(TYPES, bytes(0x10000))
        uc.mem_write(MEMBER, bytes(0x800))
        uc.mem_write(OWN, bytes(0x200))
        cap = rng.choice([0, 1, 20, 150, 250, 500, 2000, rng.randrange(0x10000)])
        uc.mem_write(GAME + 0x14e7c, struct.pack('<H', cap))
        owner = OWN
        put(MGR, owner)
        allied = [int(rng.random() < 0.3) for _ in range(256)]
        uc.mem_write(OWN + 0xac, bytes(allied[:16]))
        # Ten records; each owns a contiguous run of unit slots.
        records = []
        nextunit = 1
        damages, visibles = {}, {}
        for r in range(10):
            rec = GAME + 0x2404 + r * RSTRIDE
            active = int(rng.random() < 0.85)
            kind = rng.choice([1, 1, 2, 2, 3, 0, 4])
            idx = rng.choice([r, r, r, 10, rng.randrange(12)])
            pop = rng.choice([0, 5, 19, 20, 21, 40, 125, 300, rng.randrange(0x10000)])
            count = rng.choice([0, 1, 2, 4, 6])
            if nextunit + count > 200: count = 0
            first = UNITS + nextunit * USTRIDE
            put(rec, active); uc.mem_write(rec + 0xea, bytes([kind, idx]))
            uc.mem_write(rec + 0xe8, struct.pack('<H', pop))
            # An empty block is first > last (the scan's inclusive loop runs none).
            put(rec + 0x74, first if count else first + USTRIDE)
            put(rec + 0x78, first + (count - 1) * USTRIDE if count else first)
            units = []
            for k in range(count):
                i = nextunit + k
                u = UNITS + i * USTRIDE
                t = TYPES + i * TSTRIDE
                flags = rng.choice([0x1000000] * 8 + [0x1001000, 0]) | rng.choice([1, 1, 1, 2, 0, 3])
                if rng.random() < 0.1: flags |= 0x2000
                if rng.random() < 0.5: flags |= 0x8000000
                cloak = rng.choice([0, 0, 0, 0, 4, 1, 5])
                spread = rng.choice([30, 300, 3000])
                x = (rng.randrange(-spread, spread) + 2000) << 16 | rng.randrange(0x10000)
                z = (rng.randrange(-spread, spread) + 2000) << 16 | rng.randrange(0x10000)
                if rng.random() < 0.03: x, z = rng.getrandbits(32), rng.getrandbits(32)
                f108 = rng.choice([0.0, 0.0, 0.0, -0.0, 0.5, math.nan, 1e-30])
                mana = int(rng.random() < 0.25)
                cx, cz = rng.randrange(-5, 300), rng.randrange(-5, 300)
                put(u + 0x130, flags); uc.mem_write(u + 0x114, bytes([cloak]))
                put(u + 0x68, x); put(u + 0x70, z)
                uc.mem_write(u + 0x74, struct.pack('<hh', cx, cz))
                uc.mem_write(u + 0x108, struct.pack('<f', f108))
                put(u + 0xb4, t); put(t + 0x264, 0x80000000 if mana else 0)
                damages[i] = rng.choice([0, 0, 1, 50, 50, -2])
                visibles[i] = int(rng.random() < 0.6)
                units.append([i, flags, s32(x), s32(z), cx, cz, int(bool(cloak & 4)),
                              struct.unpack('<I', struct.pack('<f', f108))[0], mana, damages[i], visibles[i]])
            nextunit += count
            records.append([active, kind, idx, pop, len(units)] + [v for u in units for v in u])
        put(GAME + 0x14e84, UNITS); put(GAME + 0x14e88, UNITS + 200 * USTRIDE)
        # The member.
        mflags = 0x1000000 | (0x8000000 if rng.random() < 0.9 else 0) | 1
        mover = int(rng.random() < 0.95)
        nav = int(rng.random() < 0.7)
        floater = int(rng.random() < 0.2)
        depth = rng.choice([-10000, 0, 1, 12, -1])
        rang = rng.choice([0, 1, 15, 16, 17, 200, 450, -40])
        mx = (rng.randrange(1000, 3000)) << 16 | rng.randrange(0x10000)
        mz = (rng.randrange(1000, 3000)) << 16 | rng.randrange(0x10000)
        mcx, mcz = rng.randrange(0, 300), rng.randrange(0, 300)
        mfx, mfz = rng.choice([1, 2, 3]), rng.choice([1, 2, 3])
        put(MEMBER + 0x130, mflags)
        put(MEMBER + 8, MOVER if mover else 0); put(MOVER + 4, NAV if nav else 0)
        put(MEMBER + 0xc, WEAPON); put(WEAPON + 0x90, rang)
        put(MEMBER + 0x68, mx); put(MEMBER + 0x70, mz)
        uc.mem_write(MEMBER + 0x74, struct.pack('<hhhh', mcx, mcz, mfx, mfz))
        put(MEMBER + 0xb4, MTYPE); put(MTYPE + 0x260, 0x80000 if floater else 0)
        uc.mem_write(MTYPE + 0x194, struct.pack('<h', depth))
        put(MEMBER + 0xb8, owner)
        centre = [(rng.randrange(1000, 3000) << 16) | rng.randrange(0x10000), rng.getrandbits(32),
                  (rng.randrange(1000, 3000) << 16) | rng.randrange(0x10000)]
        reachable = [int(rng.random() < 0.7) for _ in range(40)]
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        case.update(damage=damages, visible=visibles, reach=reachable, reached=[], owner=owner)
        ret, error = p.call(0x40a200, (s32(centre[0]), s32(centre[1]), s32(centre[2]), MEMBER), ecx=MGR)
        if error: raise RuntimeError(error)
        result = unit_index(ret) if ret else 0
        picked += bool(ret)
        scanned += sum(1 for e in log if e[0] == 1)
        draws += sum(1 for e in log if e[0] == 0)
        reaches += len(case['reached']); refused += case['reached'].count(0)
        expected.append([result, get(0x64186c), len(log)] + [v for e in log for v in e])
        line = [cap, s32(centre[0]), s32(centre[2]), seed] + allied[:16]
        line += [mflags, mover, nav, floater, depth, rang, s32(mx), s32(mz), mcx, mcz, mfx, mfz]
        for r in records: line += r
        line += [len(reachable)] + reachable
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--pick'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d picks differ; first: case %d\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, actual[i][:60], expected[i][:60], rows[i][:1500]))
    print('PASS: %d 0x40a200 target picks, 0 mismatches: %d found a target, %d damage queries, %d draws, '
          '%d reachability queries (%d refused)' % (len(rows), picked, scanned, draws, reaches, refused))


if __name__ == '__main__':
    main()
