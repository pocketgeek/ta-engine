#!/usr/bin/env python3
"""Compare the Classic brain's combat-member planner with retail's.

Per case it runs retail's combat-member planner 0x40d370(unit, threat) for one
mobile member of a base squad, with everything it reads running as retail's own
code: the base centre 0x40c610 (anchor 0x40c750, centroid 0x40aa50), the ring
radius 0x40cf30 over the categories 0x40a5f0, the enemy query 0x411ae0 over a
random visible-enemy list (and the targeting-upgrade list), the weapon class's
vtable +0x14 (retail's own MeleeWeapon 0x531f00 or the other classes' 0x42a6b0),
the trig 0x5360bf / 0x5360f3, _ftol and the 64-bit multiplies, and the vectors'
own code. The cases cover idle and busy members (no mission, the default
mission's 0x2000, moves, attacks on a candidate, goals near one), the threat and
armed branches, every ring (the Monarch flying or not, builders, flyers), the
wounded radius around 1/4 HP, the radius fallback, melee weapons against units
on and off the ground, cloaked, unready, dying and harmless enemies, candidate
points off the map or past the reach, placement refusals, and order kinds that
do not resolve.

Logged instead of run: the placement test 0x507d10 (its cell is logged and it
answers from the case), the weapon damage 0x531e50 (a per-enemy table), the
order-kind lookup 0x4de530 (logged; it answers a kind or 0 from the case) and
the order 0x4d78a0 (kind, queue, point, mission +0x4e / +0x52). The profile home
0x412c10 is never reached (every case has a centre). operator new / delete are a
bump allocator.

The port (classic_ai_test --defence: ai::classicPlanCombat with the brain's
squad helpers, ai::classicEnemiesNear and sim::retailAiPatrolPoints) must make
the same draws, placement queries, kind lookups and orders in the same order,
and leave the same seed. check_ai_retreat.py runs the Monarch's retreat 0x40d010
on the same cases (--routine retreat here).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_defence.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

GAME = HEAP                  # +0x19e88 / +0x19e8c map size in pixels
MGR = HEAP + 0x22600         # +4 the side byte
SQOBJ = HEAP + 0x22800       # base squad: vtable, +4 manager, +8 data, +0x10 index
DATA = HEAP + 0x22900        # +0xc anchor, +0xb8.. members
VEC = HEAP + 0x22a00
BM = HEAP + 0x23000          # build manager: +9/+0xd visible list, +0x29/+0x2d, +0x79
LIST, LIST2 = HEAP + 0x23400, HEAP + 0x23600
UNITS = HEAP + 0x24000
TYPES = HEAP + 0x28000
ENEMIES = HEAP + 0x2c000
WEAPON, HANDLER, VTABLE = HEAP + 0x35000, HEAP + 0x35100, HEAP + 0x35200
MISSION = HEAP + 0x36000
STARTS, SITES = HEAP + 0x3a000, HEAP + 0x3b000
ARENA = HEAP + 0x100000
USTRIDE, TSTRIDE = 0x138, 676
MELEE, OTHER = 0x531f00, 0x42a6b0
MISSION_FLAGS = [0x05012000, 0x03000400, 0x01000280, 0x00080508, 0x03000410, 0x01002280, 0x03002400,
                 0x200, 0x400, 0x600, 0x01000600, 0x10000]


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]


def run(routine, binary, cases, plant=None):
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    log = []
    case = {}
    arena = [ARENA]

    def new(uc_, argv):
        n = get(argv)
        at = arena[0]; arena[0] += (n + 15) & ~15
        return 0, at
    def delete(uc_, argv):
        return 0, 0
    def placement(uc_, argv):
        t, selfword, cell, one, last = struct.unpack('<5I', uc_.mem_read(argv, 20))
        if t != case['type'] or selfword & 0xffff != case['self'] or one != 1 or last != 0:
            raise AssertionError('placement test arguments')
        x, z = struct.unpack('<hh', struct.pack('<I', cell))
        ok = case['place'][len(case['placed'])] if len(case['placed']) < len(case['place']) else 0
        case['placed'].append(ok)
        log.append((1, x, z))
        return 5, ok
    def damage(uc_, argv):
        return 1, case['damage'].get(get(argv), 0)
    def resolve(uc_, argv):
        out, cmd, unit, target, pos, last = struct.unpack('<6I', uc_.mem_read(argv, 24))
        if unit != UNITS or target != 0 or last != 0: raise AssertionError('kind lookup arguments')
        x, y, z = struct.unpack('<3i', uc_.mem_read(pos, 12))
        k = case['kinds'][len(case['resolved'])] if len(case['resolved']) < len(case['kinds']) else 0
        case['resolved'].append(k)
        log.append((2, cmd, x, y, z))
        uc_.mem_write(out, bytes([k])); return 6, out
    def order(uc_, argv):
        a = struct.unpack('<11I', uc_.mem_read(argv, 44))
        if a[2] != UNITS or a[3] != 0 or any(a[7:]): raise AssertionError('order arguments %s' % (a,))
        x, y, z = struct.unpack('<3i', uc_.mem_read(a[4], 12))
        log.append((3, a[0] & 0xff, a[1], x, y, z, s32(a[5]), s32(a[6])))
        return 11, 0
    def home(uc_, argv):
        raise AssertionError('the profile home 0x412c10 was reached')
    p.hooks.update({0x4eb9e0: new, 0x4eba00: delete, 0x507d10: placement, 0x531e50: damage,
                    0x4de530: resolve, 0x4d78a0: order, 0x412c10: home})
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: log.append(
        (0, s32(get(uc_.reg_read(UC_X86_REG_ESP) + 4)))), begin=0x535cc0, end=0x535cc0)

    # Branch counters (observation only): which of retail's paths the cases reach.
    hits = {}
    def count(name):
        def hook(uc_, a, size, d): hits[name] = hits.get(name, 0) + 1
        return hook
    paths = ((0x40d831, 'range point'), (0x40d8b0, 'reach point'), (0x40d9aa, 'centre point'),
             (0x40d6f3, 'on its target'), (0x40d71f, 'goal near one'), (0x40d747, 'itself near one'),
             (0x40d58a, 'candidate'), (0x40db19, 'refused point'), (0x40dbdc, 'flyer inside'),
             (0x40dc41, 'patrol lookup')) if routine == 'combat' else (
            (0x40d121, 'refused point'), (0x40d1b6, 'no enemy near'), (0x40d250, 'farther point'),
            (0x40d2c7, 'flyer leg'))
    for address, name in paths:
        uc.hook_add(UC_HOOK_CODE, count(name), begin=address, end=address)

    rng = random.Random(0x40d370 if routine == 'combat' else 0x40d010)
    rows, expected = [], []
    stats = dict(orders=0, patrols=0, candidates=0, replaced=0, early=0, wounded=0, flyers=0, nothing=0)
    put(0x62d55c, GAME)
    for n in range(cases):
        log.clear(); arena[0] = ARENA
        for base, size in ((MGR, 0x20), (SQOBJ, 0x20), (DATA, 0xc4), (BM, 0x100), (UNITS, 0x2000), (TYPES, 0x2000),
                           (ENEMIES, 0x2000), (WEAPON, 0x100), (MISSION, 0x80)):
            uc.mem_write(base, bytes(size))
        w = rng.choice([256, 800, 1600, 4000, rng.randrange(128, 8000)])
        h = rng.choice([256, 800, 1600, 4000, rng.randrange(128, 8000)])
        put(GAME + 0x19e88, w); put(GAME + 0x19e8c, h)
        side = rng.randrange(10)
        for s in range(10): put(0x62a33c + 4 * s, BM if s == side else 0)
        put(MGR + 4, side)
        put(SQOBJ, 0x5ebb2c); put(SQOBJ + 4, MGR); put(SQOBJ + 8, DATA); put(SQOBJ + 0x10, side)

        def records(base, pointer):
            out = []
            for k in range(rng.choice([0, 1, 2, 4])):
                valid = int(rng.random() < 0.9)
                x, z = rng.randrange(-2, w // 16 + 2), rng.randrange(-2, h // 16 + 2)
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((valid, x, z))
            put(pointer, base); put(pointer + 4, base + len(out) * 101)
            return out
        sites = records(SITES, 0x62dbcc)
        starts = records(STARTS, 0x62dbdc)
        anchor = 0 if rng.random() < 0.6 else rng.randrange(-4, 5)
        put(DATA + 0xc, anchor)

        # The base's members; unit 0 is the one planned.
        cx, cz = rng.randrange(0, w), rng.randrange(0, h)
        spread = rng.choice([40, 200, 800])
        nunits = rng.randrange(1, 10)
        units = []
        for i in range(nunits):
            u = UNITS + i * USTRIDE
            flags = 0x1000000 | 0x20
            if i and rng.random() < 0.05: flags |= 0x1000
            if i and rng.random() < 0.4: flags |= 0x2000000
            if rng.random() < 0.6: flags |= 0x8000000
            x = (cx + rng.randrange(-spread, spread + 1)) << 16
            z = (cz + rng.randrange(-spread, spread + 1)) << 16
            fx, fz = rng.choice([1, 2, 3, 4, 6]), rng.choice([1, 2, 3, 4, 6])
            put(u + 0x130, flags); uc.mem_write(u + 0x68, struct.pack('<iii', x, rng.randrange(0, 200 << 16), z))
            uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz)); put(u + 0xb4, TYPES + i * TSTRIDE)
            put(u + 8, 1)
            units.append([flags, x, z, fx, fz])
        members = list(range(nunits)); rng.shuffle(members)
        for k, i in enumerate(members): put(VEC + 4 * k, UNITS + i * USTRIDE)
        put(DATA + 0xb8, VEC); put(DATA + 0xbc, VEC + 4 * len(members)); put(DATA + 0xc0, VEC + 0x100)

        # The planned unit, its type, weapon and mission.
        U = UNITS
        selfword = rng.randrange(1, 0x10000)
        uc.mem_write(U + 2, struct.pack('<H', selfword))
        T = TYPES
        commander = int(rng.random() < 0.25)
        builder = int(rng.random() < 0.2)
        canfly = int(rng.random() < 0.25)
        maxdamage = rng.choice([0, 1, 100, 1000, 3999, 4000, 30000, 0xfffffff0])
        q = maxdamage >> 2
        hp = rng.choice([0, 1, q - 1, q, q + 1, -1, -300, rng.randrange(-32768, 32768)])
        hp = max(-32768, min(32767, hp))
        sight = rng.choice([0, 1, 100, 300, 600, -5, 2000])
        rang = rng.choice([0, 1, 2, 50, 200, 450, 900])
        melee = int(rng.random() < 0.3)
        put(T + 0x264, 0x40000 if commander else 0)
        put(T + 0x260, (0x100 if builder else 0) | (0x800 if canfly else 0))
        put(T + 0x1be, maxdamage); uc.mem_write(T + 0x226, struct.pack('<h', sight))
        uc.mem_write(U + 0x10c, struct.pack('<h', hp))
        put(U + 0xc, WEAPON); put(WEAPON + 0x90, rang); put(WEAPON + 0x40, HANDLER); put(HANDLER, VTABLE)
        put(VTABLE + 0x14, MELEE if melee else OTHER)
        ux, uy, uz = struct.unpack('<iii', uc.mem_read(U + 0x68, 12))

        # Enemies on the visible list (and the upgrade list).
        nen = rng.choice([0, 1, 2, 4, 8, 12])
        nen2 = rng.choice([0, 0, 1, 3])
        enemies = []
        dmg = {}
        for k in range(nen + nen2):
            e = ENEMIES + k * USTRIDE
            flags = rng.choice([0x1000000] * 6 + [0x1001000, 0x1002000, 0, 0x1000100]) | rng.choice([1, 1, 2, 0, 3])
            r = rng.choice([0, 100, 400, 900, 2000])
            ex = (cx + rng.randrange(-r - 50, r + 51)) << 16
            ez = (cz + rng.randrange(-r - 50, r + 51)) << 16
            if rng.random() < 0.05: ex, ez = rng.randrange(-1 << 31, 1 << 31), rng.randrange(-1 << 31, 1 << 31)
            ey = rng.randrange(-50 << 16, 300 << 16)
            cloak = rng.choice([0, 0, 0, 0, 4, 1])
            d = rng.choice([0, 0, 5, 100, 100, 100, -3])
            put(e + 0x130, flags); uc.mem_write(e + 0x68, struct.pack('<iii', ex, ey, ez))
            uc.mem_write(e + 0x114, bytes([cloak]))
            dmg[e] = d & 0xffffffff
            enemies.append([flags, ex, ey, ez, int(bool(cloak & 4)), d])
        for k in range(nen): put(LIST + 4 * k, ENEMIES + k * USTRIDE)
        for k in range(nen2): put(LIST2 + 4 * k, ENEMIES + (nen + k) * USTRIDE)
        first = LIST if nen or rng.random() < 0.5 else 0
        put(BM + 9, first); put(BM + 0xd, first + 4 * nen)
        put(BM + 0x29, LIST2); put(BM + 0x2d, LIST2 + 4 * nen2)
        upgrade = int(rng.random() < 0.2)
        put(BM + 0x79, upgrade)

        has = rng.random() < 0.7
        mflags = rng.choice(MISSION_FLAGS) if has else 0
        target = -1
        gx = gz = 0
        if has:
            if enemies and rng.random() < 0.5: target = rng.randrange(len(enemies))
            if enemies and rng.random() < 0.5:
                e = enemies[rng.randrange(len(enemies))]
                gx, gz = s32(e[1] + (rng.randrange(-700, 700) << 16)), s32(e[3] + (rng.randrange(-700, 700) << 16))
            else:
                gx, gz = rng.randrange(0, w) << 16, rng.randrange(0, h) << 16
            put(MISSION + 0x5a, mflags); put(MISSION + 0x16, ENEMIES + target * USTRIDE if target >= 0 else 0)
            put(MISSION + 0x22, gx); put(MISSION + 0x2a, gz)
            put(U + 0x60, MISSION)
        threat = int(rng.random() < 0.65)
        accept = rng.choice([0.0, 0.3, 0.8, 1.0, 1.0])
        place = [int(rng.random() < accept) for _ in range(60)]
        kinds = [0 if rng.random() < 0.08 else rng.randrange(1, 256) for _ in range(4)]
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        case.update(type=T, self=selfword, place=place, placed=[], kinds=kinds, resolved=[], damage=dmg)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        if routine == 'combat':
            _, error = p.call(0x40d370, (U, threat), ecx=SQOBJ)
        else:
            _, error = p.call(0x40d010, (U,), ecx=SQOBJ)
        if error: raise RuntimeError(error)
        expected.append([get(0x64186c), len(log)] + [v for e in log for v in e])
        orders = [e for e in log if e[0] == 3]
        stats['orders'] += len(orders)
        stats['patrols'] += any(e[0] == 2 and e[1] == 9 for e in log)
        stats['replaced'] += any(e[2] == 0 for e in orders)
        stats['nothing'] += not orders
        stats['flyers'] += canfly
        stats['wounded'] += (hp & 0xffffffff) < (maxdamage >> 2)
        stats['early'] += len(log) == 1 and log[0] == (0, 50)
        stats['candidates'] += bool(threat and units[0][0] & 0x8000000)
        line = [w, h, anchor, threat, seed, upgrade]
        line += [len(sites)] + [v for s in sites for v in s] + [len(starts)] + [v for s in starts for v in s]
        line += [nunits] + [v for u in units for v in u] + [len(members)] + members
        line += [commander, builder, canfly, maxdamage, hp, sight, rang, melee, ux, uy, uz]
        line += [int(has), mflags, target, gx, gz]
        line += [nen, nen2] + [v for e in enemies for v in e]
        line += [len(place)] + place + [len(kinds)] + kinds
        rows.append(' '.join(map(str, line)))

    flag = '--defence' if routine == 'combat' else '--retreat'
    out = subprocess.run([binary, flag], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d calls differ; first: case %d\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, actual[i], expected[i], rows[i][:2000]))
    stats['paths'] = ', '.join('%s %d' % kv for kv in sorted(hits.items()))
    return len(rows), stats


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--routine', choices=('combat', 'retreat'), default='combat')
    args = ap.parse_args()
    n, stats = run(args.routine, args.binary, args.cases)
    name = '0x40d370 combat-member planner' if args.routine == 'combat' else '0x40d010 Monarch retreat'
    print('PASS: %d %s calls, 0 mismatches: %d orders, %d calls with a patrol lookup, %d replacing a mission, '
          '%d issuing nothing, %d stopped by the first rand(50), %d armed under threat, %d wounded, %d flyers; '
          'retail paths reached: %s'
          % (n, name, stats['orders'], stats['patrols'], stats['replaced'], stats['nothing'], stats['early'],
             stats['candidates'], stats['wounded'], stats['flyers'], stats['paths']))


if __name__ == '__main__':
    main()
