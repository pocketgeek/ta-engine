#!/usr/bin/env python3
"""Compare the Classic brain's constructor planner with retail's.

Per case it runs retail's constructor planner 0x40c890 for one mobile builder of
a base squad, with a random threat answer, and everything the planner reads
running as retail's own code: the base centre 0x40c610 (anchor 0x40c750,
centroid 0x40aa50), the ring radius 0x40cf30, the member counts 0x40a880 and
firepower 0x40a9b0 over the categories 0x40a5f0, the Monarch's shot cost
0x531ea0 / 0x5193c0, the mission descriptors 0x4d4cc0 (a small fake table, so a
mission's class is the case's own), the type count 0x410e60, the menu draw
0x412d00 with the entry weight 0x412c90 and limit check 0x409890 -> 0x410de0,
the busy-deposit test 0x40c830, and the site search 0x412f80 -> 0x411510 /
0x411200 on a random feature plane and sacred-site table with the case's block
grids. The cases cover the Monarch's HP / mana / castable retreat thresholds
(around their edges, with HP of either sign), the threat and ground-firepower
branches, the builder gate against the base's p1, the alarm deadline, the
allocation 0.7 line (0.7f itself counts as below), builders already building,
attack-class first missions, the duplicate-factory rule (members of the picked
type idle, busy on BuildingBuild or on another mission), the Monarch's 4-tower
cap and the ring choice, unreachable sites, and the lodestone pass replacing
the ordinary pick.

Logged instead of run: the placement test 0x507d10 (it refuses a footprint off
the map and otherwise answers at random), the reachability test 0x4e1570 (its
arguments are logged and it answers at random; the routine has its own oracle,
check_rectangle_reachability.py), the order-kind lookup 0x4de530 (kind 14; it
always resolves), the order 0x4d78a0 (type and site), the mission clear
0x4d6a50, the Monarch's retreat 0x40d010 and the defence planner 0x40d370 (later
phases; the oracle checks that and when they are reached). The mission-name
lookup 0x4d4bf0 resolves BuildingBuild to the case's kind.

The port (classic_ai_test --constructor: ai::classicPlanConstructor with the
brain's squad helpers classicBaseCentre / classicBaseRadius / classicBaseCount /
classicBaseFirepower / classicIdleFactory / classicLodestoneBusy, the shared menu
draw and weights, classicShotCost, classicReachRect and the placers) must make
the same draws, placement and reachability queries, orders and planner calls in
the same order, and leave the same seed. The FPU runs at the C runtime's 53-bit
precision (0x027f), as in check_ai_profile.py.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_constructor.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESP, UC_X86_REG_ECX

GAME = HEAP                  # +0x19f44 tick, +0x175b8/+0x175c4 types, map fields
PLAYER = HEAP + 0x22000      # +0x10c resource object
RES = HEAP + 0x22400
MGR = HEAP + 0x22600         # [0] player, +0xd the Monarch alarm deadline
SQOBJ = HEAP + 0x22800
DATA = HEAP + 0x22900        # +0xc anchor, +0x10 p1, +0xb8.. members
VEC = HEAP + 0x22a00
BM = HEAP + 0x23000          # build manager: tables, block grids at +0x105 / +0x111
TABLES = HEAP + 0x23200
UNITS = HEAP + 0x24000
TYPES = HEAP + 0x28000
MENUS = HEAP + 0x30000
STRINGS = HEAP + 0x31000
MISSIONS = HEAP + 0x32000
MTAB = HEAP + 0x34000
WEAPONS = HEAP + 0x35000
NAV, NAVFLY = HEAP + 0x39000, HEAP + 0x39100
STARTS, SITES = HEAP + 0x3a000, HEAP + 0x3b000
FTYPES = HEAP + 0x40000
CELLS = HEAP + 0x50000
USTRIDE, TSTRIDE = 0x138, 676
BUILDINGBUILD = 3
MISSION_CLASS = [0, 1, 2, 19, 14, 7, 6, 5]
SIDES = [b'Aramon\0', b'Veruna\0', b'aramon\0']
MARKERS = [0xfffa, 0xfffb, 0xfffc, 0xfffd, 0xffff]


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


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

    def unpack16(v):
        return struct.unpack('<hh', struct.pack('<I', v & 0xffffffff))

    def placement(uc_, argv):
        t, zero, cell, one, last = struct.unpack('<5I', uc_.mem_read(argv, 20))
        if zero != 0 or one != 1 or last != 0: raise AssertionError('placement test arguments')
        x, z = unpack16(cell)
        fx, fz = struct.unpack('<hh', uc_.mem_read(t + 0x126, 4))
        inside = 0 <= x and 0 <= z and x + fx <= case['w'] and z + fz <= case['h']
        ok = int(inside and case['rng'].random() < case['accept'])
        log.append((1, x, z)); case['place'].append(ok)
        return 5, ok
    def reach(uc_, argv):
        src, size, pos, rect, mode, limited = struct.unpack('<6I', uc_.mem_read(argv, 24))
        if size != 0x10001 or mode != 1 or limited != 0: raise AssertionError('reachability arguments')
        if uc_.reg_read(UC_X86_REG_ECX) == 0: raise AssertionError('reachability without a grid')
        ok = int(case['rng'].random() < case['reach'])
        log.append((2, *unpack16(src), *unpack16(pos), *unpack16(rect))); case['reached'].append(ok)
        return 6, ok
    def resolve(uc_, argv):
        out, kind = struct.unpack('<II', uc_.mem_read(argv, 8))
        if kind != 14: raise AssertionError('order kind %d' % kind)
        uc_.mem_write(out, bytes([0x0e])); return 6, out
    def issue(uc_, argv):
        a = struct.unpack('<11I', uc_.mem_read(argv, 44))
        if a[0] & 0xff != 0x0e or a[6] != 1: raise AssertionError('order arguments %s' % (a,))
        log.append((3, a[5] & 0xffff, struct.unpack('<i', uc_.mem_read(a[4], 4))[0],
                    struct.unpack('<i', uc_.mem_read(a[4] + 8, 4))[0]))
        return 11, 0
    def combat(uc_, argv):
        u, t = struct.unpack('<II', uc_.mem_read(argv, 8)); log.append((4, t & 0xff)); return 2, 0
    def retreat(uc_, argv):
        log.append((5,)); return 1, 0
    def cancel(uc_, argv):
        u, k = struct.unpack('<II', uc_.mem_read(argv, 8))
        if k != 0: raise AssertionError('mission clear %d' % k)
        log.append((6,)); return 2, 0
    def identify(uc_, argv):
        if get(argv) != 0x604914: raise AssertionError('unexpected mission name %#x' % get(argv))
        uc_.mem_write(uc_.reg_read(UC_X86_REG_ECX), bytes([BUILDINGBUILD])); return 1, uc_.reg_read(UC_X86_REG_ECX)
    p.hooks.update({0x507d10: placement, 0x4e1570: reach, 0x4de530: resolve, 0x4d78a0: issue, 0x40d370: combat,
                    0x40d010: retreat, 0x4d6a50: cancel, 0x4d4bf0: identify})
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: log.append(
        (0, struct.unpack('<i', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP) + 4, 4))[0])), begin=0x535cc0, end=0x535cc0)

    # Branch counters (observation only): which of retail's paths the cases reach.
    hits = {}
    def count(name, test=None):
        def hook(uc_, a, size, d):
            if test is None or test(uc_): hits[name] = hits.get(name, 0) + 1
        return hook
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX
    for address, name, test in (
            (0x40cbe2, 'dupdrop', None), (0x40ca54, 'alarm', None), (0x411510, 'deposit', None),
            (0x411200, 'scatter', None), (0x40cb93, 'tower', None),
            (0x40ca37, 'gate', lambda u: struct.unpack('<i', struct.pack('<I', u.reg_read(UC_X86_REG_EAX)))[0] >=
                struct.unpack('<i', u.mem_read(u.reg_read(UC_X86_REG_EBX), 4))[0]),
            (0x40cbac, 'towercap', lambda u: struct.unpack('<i', struct.pack('<I', u.reg_read(UC_X86_REG_EAX)))[0] >= 4),
            (0x40cd37, 'lodepick', None)):
        uc.hook_add(UC_HOOK_CODE, count(name, test), begin=address, end=address)

    rng = random.Random(0x40c890)
    rows, expected = [], []
    stats = {k: 0 for k in ('monarch', 'retreat', 'cancel', 'build', 'lodestone', 'lowfunds', 'deposit', 'dupdrop',
                            'towercap', 'gate', 'alarm', 'unreachable', 'attackbusy', 'alreadybuilding')}
    put(0x62d55c, GAME)
    uc.mem_write(MTAB, b''.join(bytes(16) + bytes([c]) + bytes(8) for c in MISSION_CLASS))
    put(0x62db84, MTAB)
    for n in range(args.cases):
        log.clear()
        uc.mem_write(GAME, bytes(0x1a000)); uc.mem_write(PLAYER, bytes(0x200)); uc.mem_write(RES, bytes(0x40))
        uc.mem_write(MGR, bytes(0x1b0)); uc.mem_write(DATA, bytes(0xc4)); uc.mem_write(BM, bytes(0x200))
        uc.mem_write(UNITS, bytes(0x4000)); uc.mem_write(TYPES, bytes(0x8000)); uc.mem_write(MISSIONS, bytes(0x2000))
        uc.mem_write(WEAPONS, bytes(0x2000)); uc.mem_write(NAV, bytes(0x200))
        put(NAV + 4, HEAP + 0x39800)
        tick = rng.choice([0, 1000, rng.randrange(1, 1 << 20)])
        alarm = rng.choice([0, 0, tick, tick + 1, max(tick - 1, 0), tick + rng.randrange(1, 900)])
        put(GAME + 0x19f44, tick); put(MGR + 0xd, alarm)
        index = rng.choice([0, 1, 2, 5, 9, 10])
        limits = rng.random() < 0.8
        if index < 10: uc.mem_write(GAME + index * 0x110 + 0x24e7, bytes([int(limits)]))
        limited = index < 10 and limits
        put(0x62a33c + index * 4, BM)
        put(PLAYER + 0x10c, RES)
        alloc = f32(rng.choice([1.0] * 8 + [0.7, 0.70000005, 0.69, 0.5, 0.2, 0.0]))
        uc.mem_write(RES + 8, struct.pack('<f', alloc))
        threat = int(rng.random() < 0.3)

        # Block grids, map, sacred sites.
        grids = []
        for base, margin, sx, sz in ((0x105, 6, 10, 3), (0x111, 9, 20, 3)):
            stepx, stepz = 8 + margin + rng.randrange(sx), 8 + margin + rng.randrange(sz)
            g = [stepx, stepz, rng.randrange(stepx) - stepx // 2, rng.randrange(stepz) - stepz // 2, margin]
            uc.mem_write(BM + base, struct.pack('<hhhhi', *g)); grids += g
        w, h = rng.randrange(32, 64), rng.randrange(32, 64)
        put(GAME + 0x19e98, w); put(GAME + 0x19e9c, h); put(GAME + 0x19f04, CELLS)
        uc.mem_write(CELLS, (bytes(8) + b'\xff\xff' + bytes(4)) * (w * h))
        nft = rng.randrange(1, 6)
        sacred = [rng.choice([0.0, 1.0, 1.0, 2.0, float('nan')]) for _ in range(nft)]
        put(GAME + 0x19ec0, nft); put(GAME + 0x19edc, FTYPES)
        # Past the count the table reads 0 (retail's back-reference path does not
        # check the count; it would read whatever follows the table).
        uc.mem_write(FTYPES, bytes(320 * 8))
        for i, v in enumerate(sacred): uc.mem_write(FTYPES + i * 320 + 0x134, struct.pack('<f', v))
        cells, anchors = {}, []
        density = rng.choice([0.0, 0.02, 0.08])
        for z in range(h):
            for x in range(w):
                if rng.random() >= density: continue
                r = rng.random()
                if r < 0.6: cells[(x, z)] = (rng.randrange(nft + 1), 0, 0); anchors.append((x, z))
                elif r < 0.9 and anchors:
                    ax, az = rng.choice(anchors)
                    if ax <= x and az <= z: cells[(x, z)] = (0xfffe, x - ax, z - az)
                else: cells[(x, z)] = (rng.choice(MARKERS), 0, 0)
        for (x, z), (f, bx, bz) in cells.items():
            uc.mem_write(CELLS + (z * w + x) * 14 + 8, struct.pack('<HBB', f, bz, bx))
        def records(base, pointer, pool):
            out = []
            for k in range(rng.choice([0, 1, 2, 4, 8])):
                valid = int(rng.random() < 0.9)
                if pool and rng.random() < 0.7: x, z = rng.choice(pool)
                else: x, z = rng.randrange(-2, w + 2), rng.randrange(-2, h + 2)
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((valid, x, z))
            put(pointer, base); put(pointer + 4, base + len(out) * 101)
            return out
        sites = records(SITES, 0x62dbcc, anchors)
        starts = records(STARTS, 0x62dbdc, None)

        # Types.
        ntypes = rng.randrange(3, 13)
        put(GAME + 0x175b8, ntypes); put(GAME + 0x175c4, TYPES)
        uc.mem_write(STRINGS, b''.join(SIDES)); uc.mem_write(STRINGS + 0x40, bytes([0x8f, 0x0f, 0x80, 0x00]))
        prio_t, count_t, pref_t, limit_t = TABLES, TABLES + 0x40, TABLES + 0x80, TABLES + 0xc0
        put(BM + 0x69, prio_t); put(BM + 0x85, count_t); put(BM + 0xc5, pref_t); put(BM + 0xe5, limit_t)
        types = []
        towers = rng.random() < 0.15        # a Monarch whose menu is mostly towers near the cap
        for t in range(ntypes):
            rec = TYPES + t * TSTRIDE
            pref = rng.choice([0, 50, 100, 100, 100, 100])
            prio = rng.choice([-1, 0, 1, 21, 42, 84, 84, 100, 100])
            limit = rng.choice([-1, -1, -1, -1, 0, 1, 4, rng.randrange(-2, 20)])
            count = rng.choice([0, 0, 1, 2, 3, 4, 5, -1])
            yard = rng.choice([-1, -1, -1, 0x8f, 0x8f, 0x0f])
            side = rng.choice([0, 0, 0, 0, 0, 1, 2])
            cost = f32(rng.choice([0, 100, 500, 800, 2000]))
            commander = rng.random() < 0.3
            menu = [] if t == 0 or rng.random() < 0.5 else [rng.randrange(1, ntypes) for _ in range(rng.choice([1, 2, 3, 5, 8]))]
            menu_field = rng.choice([0, 0, 0, 0, 0, 1, 3, -1]) if not menu else len(menu)
            bm = rng.choice([0, 0, 0, 1])
            armed = int(rng.random() < 0.5)
            rang = rng.choice([0, 300, 999, 1000, 1500])
            fx, fz = rng.choice([1, 2, 2, 3, 4]), rng.choice([1, 2, 2, 3, 4])
            mwd = rng.choice([-10000, -10000, -10000, 0])
            reachd = rng.choice([0, 15, 16, 64, 90, 120, 250, 65535])
            maxdamage = rng.choice([1000, 2000, 3500, 4000, 1, 0])
            manasite = int(yard == 0x8f or rng.random() < 0.1)
            unitcost = int(rng.random() < 0.2)
            canfly, floater = int(rng.random() < 0.15), int(rng.random() < 0.1)
            if towers:
                if menu: commander = True
                else: bm, armed, menu_field, count = 0, 1, 0, rng.choice([2, 3, 4, 4, 5])
            uc.mem_write(prio_t + t, struct.pack('<b', prio)); uc.mem_write(count_t + 2 * t, struct.pack('<h', count))
            uc.mem_write(pref_t + t, bytes([pref])); uc.mem_write(limit_t + 4 * t, struct.pack('<i', limit))
            put(rec + 0x8a, STRINGS + 7 * side)
            put(rec + 0x12a, 0 if yard < 0 else STRINGS + 0x40 + {0x8f: 0, 0x0f: 1}[yard])
            uc.mem_write(rec + 0x20e, struct.pack('<f', cost))
            put(rec + 0x264, (0x40000 if commander else 0) | (0x80000000 if manasite else 0))
            put(rec + 0x260, (0x10000 if armed else 0) | (0x800 if canfly else 0) | (0x80000 if floater else 0))
            uc.mem_write(rec + 0x268, bytes([0x10 if unitcost else 0]))
            put(rec + 0x12e, menu_field)
            for k, e in enumerate(menu): uc.mem_write(MENUS + t * 0x40 + 2 * k, struct.pack('<H', e))
            put(rec + 0x132, MENUS + t * 0x40)
            uc.mem_write(rec + 0x24a, bytes([bm]))
            put(rec + 0x1aa, WEAPONS + 0x1800 + t * 0x40); put(WEAPONS + 0x1800 + t * 0x40 + 0x90, rang)
            uc.mem_write(rec + 0x126, struct.pack('<hh', fx, fz)); uc.mem_write(rec + 0x194, struct.pack('<h', mwd))
            uc.mem_write(rec + 0x230, struct.pack('<H', reachd)); put(rec + 0x1be, maxdamage)
            types.append(dict(pref=pref, prio=prio, limit=limit, count=count, yard=yard, side=side, cost=bits(cost),
                              commander=int(commander), menu_field=menu_field, menu=menu, bm=bm, armed=armed,
                              range=rang, fx=fx, fz=fz, mwd=mwd, reach=reachd, maxdamage=maxdamage,
                              manasite=manasite, unitcost=unitcost, canfly=canfly, floater=floater))

        # The base, its units and the builder.
        slot = rng.choice([1, 1, 2, 7, 20])
        anchor = 0 if rng.random() < 0.5 else rng.randrange(-4, 5)
        p1 = rng.choice([0, 1, 2, 3, 5])
        put(MGR, PLAYER); uc.mem_write(MGR + 4, bytes([index]))
        put(SQOBJ, 0x5ebb2c); put(SQOBJ + 4, MGR); put(SQOBJ + 8, DATA); put(SQOBJ + 0x10, index)
        put(DATA + 0xc, anchor); put(DATA + 0x10, p1)
        builders = [t for t in range(1, ntypes) if types[t]['menu']]
        if not builders: continue
        nunits = rng.randrange(1, 12)
        units = []
        for i in range(nunits):
            u = UNITS + i * USTRIDE
            t = rng.choice(builders) if i == 0 else rng.randrange(ntypes)
            flags = 0x1000000 | 0x20
            if i and rng.random() < 0.05: flags |= 0x1000
            if rng.random() < 0.4 and i: flags |= 0x2000000
            if rng.random() < 0.5: flags |= 0x8000000
            mover = 1 if i == 0 else int(rng.random() < 0.7)
            x, z = rng.randrange(0, w * 16) << 16, rng.randrange(0, h * 16) << 16
            fx, fz = rng.choice([1, 2, 3, 4]), rng.choice([1, 2, 3, 4])
            has = rng.random() < (0.3 if i == 0 else 0.55)
            kind = rng.randrange(len(MISSION_CLASS)) if has else 0
            f5a = rng.choice([0, 8, 0x0c, 0x80, 0x10, 0x88]) if has else 0
            target = rng.randrange(ntypes)
            damage, reload = rng.choice([0, 5, 30, 200, 65535]), rng.choice([0, 1, 30, 45, 90])
            uc.mem_write(u, struct.pack('<H', t))
            put(u + 0xb4, TYPES + t * TSTRIDE); put(u + 0xb8, PLAYER); put(u + 0x130, flags); put(u + 0xc8, slot)
            nav = (NAV if i else rng.choice([NAV, NAV, NAVFLY])) if mover else 0
            put(u + 8, nav)
            put(u + 0x68, x); put(u + 0x70, z); uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz))
            wpn = WEAPONS + i * 0x100
            put(u + 0xc, wpn); uc.mem_write(wpn + 0x88, struct.pack('<H', damage))
            uc.mem_write(wpn + 0x9c, struct.pack('<H', reload))
            if has:
                m = MISSIONS + i * 0x100
                uc.mem_write(m + 4, bytes([kind])); put(m + 0x4e, target); put(m + 0x5a, f5a); put(m + 0x66, 0)
                put(u + 0x60, m)
            units.append([flags, t, x, z, fx, fz, int(has), MISSION_CLASS[kind] if has else 0, f5a,
                          int(has and kind == BUILDINGBUILD), types[target]['manasite'] if has else 0,
                          int(nav != 0), damage, reload])
        b = UNITS
        bt = types[units[0][1]]
        md = bt['maxdamage']
        hp = rng.choice([0, 1, md // 8 - 1, md // 8, md // 4 - 1, md // 4, md // 2 - 1, md // 2, md, -5,
                         rng.randrange(-32768, 32768)])
        maxmana = f32(rng.choice([100.0, 100.0, 0.0, 50.0]))
        mana = f32(rng.choice([0.0, 50.0, 50.000004, 49.0, 100.0, 10.0]))
        weapon = int(rng.random() < 0.7)
        wcost = f32(rng.choice([0.0, 0.005, 0.01, 10.0, 50.0, 60.0, 200.0]))
        mult = f32(rng.choice([1.0, 1.1, 1.5, 0.5]))
        cell = (rng.randrange(0, w), rng.randrange(0, h))
        uc.mem_write(b + 0x10c, struct.pack('<h', hp))
        uc.mem_write(b + 0xd8, struct.pack('<ff', mana, maxmana)); uc.mem_write(b + 0xf4, struct.pack('<f', mult))
        put(b + 0x44, WEAPONS + 0x1f00 if weapon else 0); uc.mem_write(WEAPONS + 0x1f00 + 0xd4, struct.pack('<f', wcost))
        uc.mem_write(b + 0x74, struct.pack('<hh', *cell))
        navigator = int(get(b + 8) == NAV)
        members = list(range(nunits)); rng.shuffle(members)
        if rng.random() < 0.2 and nunits > 1: members.remove(rng.randrange(1, nunits))
        for k, i in enumerate(members): put(VEC + 4 * k, UNITS + i * USTRIDE)
        put(DATA + 0xb8, VEC); put(DATA + 0xbc, VEC + 4 * len(members)); put(DATA + 0xc0, VEC + 0x100)
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        case.update(w=w, h=h, accept=rng.choice([0.0, 0.3, 0.8, 1.0, 1.0]), reach=rng.choice([0.0, 0.5, 1.0, 1.0]),
                    rng=random.Random(rng.getrandbits(32)), place=[], reached=[])

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x40c890, (b, threat), ecx=SQOBJ)
        if error: raise RuntimeError(error)
        row = [get(0x64186c), len(log)] + [v for e in log for v in e]
        expected.append(row)
        kinds = [e[0] for e in log]
        stats['monarch'] += bt['commander']
        stats['retreat'] += 5 in kinds
        stats['cancel'] += 6 in kinds
        stats['build'] += 3 in kinds
        stats['lodestone'] += any(e[0] == 3 and types[e[1]]['yard'] == 0x8f for e in log)
        stats['lowfunds'] += alloc < 0.7
        stats['unreachable'] += 0 in case['reached']
        line = [tick, alarm, bits(alloc), threat, int(limited), slot, anchor, p1, seed, 0] + grids
        line += [w, h, nft] + [bits(v) for v in sacred] + [len(cells)]
        line += [v for (x, z), (f, bx, bz) in cells.items() for v in (x, z, f, bx, bz)]
        line += [len(sites)] + [v for s in sites for v in s] + [len(starts)] + [v for s in starts for v in s]
        line += [ntypes]
        for t in types:
            line += [t['pref'], t['prio'], t['limit'], t['count'], t['yard'], t['side'], t['cost'], t['commander'],
                     t['menu_field'], len(t['menu'])] + t['menu']
            line += [t['bm'], t['armed'], t['range'], t['fx'], t['fz'], t['mwd'], t['reach'], t['maxdamage'],
                     t['manasite'], t['unitcost'], t['canfly'], t['floater']]
        line += [nunits] + [v for u in units for v in u]
        line += [hp, bits(mana), bits(maxmana), weapon, bits(wcost), bits(mult), cell[0], cell[1], navigator]
        line += [len(members)] + members + [len(case['place'])] + case['place'] + [len(case['reached'])] + case['reached']
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--constructor'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d planner calls differ; first: case %d\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, actual[i], expected[i], rows[i][:2000]))
    print('PASS: %d constructor planner calls, 0 mismatches: %d by a Monarch, %d retreats, %d builds cancelled, '
          '%d builds ordered (%d lodestones), %d under allocation 0.7, %d with an unreachable site; retail paths '
          'reached: %s' % (len(rows), stats['monarch'], stats['retreat'], stats['cancel'], stats['build'],
                           stats['lodestone'], stats['lowfunds'], stats['unreachable'],
                           ', '.join('%s %d' % kv for kv in sorted(hits.items()))))


if __name__ == '__main__':
    main()
