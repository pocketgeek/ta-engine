#!/usr/bin/env python3
"""Compare the Classic brain's army squads with retail's.

Per case it runs one of retail's army-squad thinks on a random player:
--routine strike (the default) the strike squad's 0x40e180, backup its
backup's 0x40ea30, vtol a VTOL squad's 0x40eb40. Each case has up to 15 own
units spread over the strike, its backup, a VTOL squad, bases 1-3 and no squad
(live, dying, selectable or not, structures, armed or not, flyers, floaters,
builders, the Monarch; with or without a mover and a ground navigator; at hp
around every threshold the routines test; with or without a first mission of
various kinds, classes, flags and goals), a few enemy units for the strike's
target (live, dying, empty slots), a visible-enemy list, base anchors on start
and sacred-site records, the strike's state (backup slot, cohesion, dissolve
gate, target, p5), the tick and the map size.

Everything under the routines is retail's own code: the cohesion pass 0x40abd0,
set-squad 0x50b4f0 (with its vector code), the retreat base 0x40e120 / 0x40ee50
(firepower 0x40a9b0, nearest base 0x40af70, base centre 0x40c610 with its
anchors, centroid 0x40aa50, categories 0x40a5f0), the member count 0x40de60,
the target lookup 0x40ddd0 / 0x40de20, the threat test 0x412000, the ring
radius 0x40cf30, set-active 0x50b700, the group order 0x50b5d0, _allmul and the
game RNG. Logged instead of run: the target picker 0x40a200 (it has its own
oracle, check_ai_target_pick.py; it answers from the case), the mission clear
0x4d6a50, the order-kind lookup 0x4de530 (it answers a kind or 0 from the
case), the order 0x4d78a0, the reachability walk 0x4e1570 (it answers from the
case) and the mission-descriptor lookup 0x4d4cc0 (it answers the case's class).

The port (classic_ai_test --strike / --backup / --vtol: the shared
sim::retailTickAiSquad clock template with ai::classicStrikeRecruit and
ai::classicStrikeThink, ai::classicBackupThink or ai::classicVtolThink under
it) must make the same draws, queries and orders in the same order and leave
the same seed, deadline, unit squads, member lists in order and squad state.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_strike.py --binary build-o2/classic_ai_test [--routine strike|backup|vtol]
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_FPCW

GAME = HEAP                  # +0x19f44 tick, +0x14e84/88 unit table, +0x19e88/8c map size
PLAYER = HEAP + 0x22000      # +0x74/+0x78 own units, +0x84 squad data
MGR = HEAP + 0x22400         # [mgr] player, +4 side, +0x11 squad objects
BM = HEAP + 0x22600          # build manager: +9/+0xd visible-enemy list
LIST = HEAP + 0x22800
SQOBJ = HEAP + 0x23000       # 100 squad objects, 0x20 apart
DATA = HEAP + 0x24000        # 100 squad data records, 0xc4 apart
VEC = HEAP + 0x29000         # member storage, 64 pointers per slot
UNITS = HEAP + 0x30000
TYPES = HEAP + 0x40000
WEAPONS = HEAP + 0x48000
MISSIONS = HEAP + 0x4c000
MOVERS, NAVS = HEAP + 0x50000, HEAP + 0x51000
DESC = HEAP + 0x52000
STARTS, SITES = HEAP + 0x53000, HEAP + 0x54000
ARENA = HEAP + 0x100000
USTRIDE, TSTRIDE, DSTRIDE, CAP = 0x138, 0x2a4, 0xc4, 64
VTABLE = {'base': 0x5ebb2c, 'strike': 0x5ebb50, 'backup': 0x5ebb60, 'vtol': 0x5ebb70, 'reserve': 0x5ebb1c}
THINK = {'strike': 0x40e180, 'backup': 0x40ea30, 'vtol': 0x40eb40}
FLAGS = [0x3000400, 0x3000400, 0x3010400, 0x10000, 0x2000, 0x3002400, 0x400, 0, 0x1000280, 0x80508, 0x10400]


def kind(slot):
    if slot <= 20: return 'base'
    if slot <= 80: return 'strike' if slot % 2 else 'backup'
    if slot <= 95: return 'vtol'
    return {96: 'reserve', 97: 'strike', 98: 'backup'}[slot]


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]


def run(routine, binary, cases):
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
    heap = [ARENA]
    log = []
    case = {}

    def index(address):
        return (address - UNITS) // USTRIDE if address else 0
    def new(uc_, argv):
        size = get(argv); at = heap[0]; heap[0] = (at + size + 15) & ~15
        return 0, at
    def pick(uc_, argv):
        if uc_.reg_read(UC_X86_REG_ECX) != MGR: raise AssertionError('picker manager')
        x, y, z, member = struct.unpack('<iiiI', uc_.mem_read(argv, 16))
        log.append((6, index(member), x, z))
        t = case['pick']
        return 4, UNITS + t * USTRIDE if t else 0
    def clear(uc_, argv):
        u, arg = struct.unpack('<II', uc_.mem_read(argv, 8))
        log.append((4, index(u), arg)); return 2, 0
    def resolve(uc_, argv):
        out, cmd, u, target, pos, last = struct.unpack('<6I', uc_.mem_read(argv, 24))
        if last != 0: raise AssertionError('kind lookup arguments')
        x, y, z = struct.unpack('<3i', uc_.mem_read(pos, 12))
        k = case['kinds'][case['resolved']] if case['resolved'] < len(case['kinds']) else 0
        case['resolved'] += 1
        log.append((2, cmd, index(u), target, x, z))
        uc_.mem_write(out, bytes([k])); return 6, out
    def order(uc_, argv):
        a = struct.unpack('<11I', uc_.mem_read(argv, 44))
        if any(a[7:]): raise AssertionError('order arguments %s' % (a,))
        x, y, z = struct.unpack('<3i', uc_.mem_read(a[4], 12))
        log.append((3, a[0] & 0xff, a[1], index(a[2]), a[3], x, z, s32(a[5]), s32(a[6])))
        return 11, 0
    def reach(uc_, argv):
        a = struct.unpack('<6I', uc_.mem_read(argv, 24))
        sx, sz = struct.unpack('<hh', struct.pack('<I', a[0]))
        fx, fz = struct.unpack('<hh', struct.pack('<I', a[1]))
        tx, tz = struct.unpack('<hh', struct.pack('<I', a[2]))
        rx, rz = struct.unpack('<hh', struct.pack('<I', a[3]))
        k = case['reached']; case['reached'] += 1
        ok = case['reach'][k] if k < len(case['reach']) else 1
        log.append((5, sx, sz, fx, fz, tx, tz, rx, rz, a[4], a[5] & 0xff))
        return 6, ok
    def descriptor(uc_, argv):
        m = uc_.reg_read(UC_X86_REG_ECX) - 4
        return 0, DESC + case['classes'][(m - MISSIONS) // 0x80] * 0x20
    p.hooks.update({0x4eb9e0: new, 0x4eba00: lambda uc_, argv: (0, 0), 0x40a200: pick, 0x4d6a50: clear,
                    0x4de530: resolve, 0x4d78a0: order, 0x4e1570: reach, 0x4d4cc0: descriptor})
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: log.append(
        (0, s32(get(uc_.reg_read(UC_X86_REG_ESP) + 4)))), begin=0x535cc0, end=0x535cc0)
    for c in range(32):
        uc.mem_write(DESC + c * 0x20, bytes(0x10) + bytes([c]) + bytes(0xf))
    # Branch counters (observation only): which of retail's paths the cases reach.
    hits = {}
    def count(name):
        def hook(uc_, a, size, d): hits[name] = hits.get(name, 0) + 1
        return hook
    paths = {'strike': ((0x40ac46, 'seeded'), (0x40adde, 'demoted'), (0x40aef0, 'promoted'), (0x40e24c, 'dissolved'),
                        (0x40e334, 'repick'), (0x40e3a7, 'changed'), (0x40e3b8, 'no target'), (0x40e45b, 'wounded'),
                        (0x40e4fe, 'unreachable'), (0x40e528, 'unarmed'), (0x40e5f1, 'sent home'),
                        (0x40e8d6, 'contact'), (0x40e72f, 'march'), (0x40e808, 'close'), (0x40e86d, 'forced draw'),
                        (0x40e8b2, 'march order'), (0x40e964, 'contact order')),
             'backup': ((0x50b63e, 'group order'),),
             'vtol': ((0x40ece0, 'sent home'), (0x40ed48, 'busy draw'), (0x40ed65, 'anywhere'),
                      (0x40edbb, 'top'), (0x40edc8, 'bottom'), (0x40edfa, 'left'), (0x40ee02, 'right'),
                      (0x50b63e, 'patrol order'))}[routine]
    for address, name in paths:
        uc.hook_add(UC_HOOK_CODE, count(name), begin=address, end=address)

    rng = random.Random({'strike': 0x40e180, 'backup': 0x40ea30, 'vtol': 0x40eb40}[routine])
    rows, expected = [], []
    stats = dict(orders=0, clears=0, picks=0, contact=0, march=0, none=0, reach=0)
    put(0x62d55c, GAME)
    for n in range(cases):
        heap[0] = ARENA; log.clear()
        uc.mem_write(GAME, bytes(0x20000)); uc.mem_write(PLAYER, bytes(0x200)); uc.mem_write(MGR, bytes(0x1b0))
        uc.mem_write(BM, bytes(0x100))
        for base, size in ((UNITS, 0x4000), (TYPES, 0x8000), (WEAPONS, 0x2000), (MISSIONS, 0x2000), (MOVERS, 0x400)):
            uc.mem_write(base, bytes(size))
        tick = rng.choice([0, 1000, 54321, 0xfffffff0, rng.getrandbits(32)])
        put(GAME + 0x19f44, tick)
        w = rng.choice([256, 800, 1600, 4000, rng.randrange(1, 9000)])
        h = rng.choice([256, 800, 1600, 4000, rng.randrange(1, 9000)])
        put(GAME + 0x19e88, w); put(GAME + 0x19e8c, h)
        side = rng.randrange(10)
        for s in range(10): put(0x62a33c + 4 * s, BM if s == side else 0)
        put(MGR, PLAYER); uc.mem_write(MGR + 4, bytes([side]))
        put(PLAYER + 0x84, DATA)
        uc.mem_write(DATA, bytes(100 * DSTRIDE))
        for slot in range(100):
            _, error = p.call(0x50b340, (PLAYER, slot), ecx=DATA + slot * DSTRIDE)
            if error: raise RuntimeError(error)
        for slot in range(100):
            obj = SQOBJ + slot * 0x20
            if slot in (0, 99):
                put(MGR + 0x11 + slot * 4, 0); continue
            put(obj, VTABLE[kind(slot)]); put(obj + 4, MGR); put(obj + 8, DATA + slot * DSTRIDE)
            put(obj + 12, 0); put(obj + 16, side)
            put(MGR + 0x11 + slot * 4, obj)

        def records(base, pointer):
            out = []
            for k in range(rng.choice([0, 1, 2, 4])):
                valid = int(rng.random() < 0.9)
                x, z = rng.randrange(0, 200), rng.randrange(0, 200)
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((valid, x, z))
            put(pointer, base); put(pointer + 4, base + len(out) * 101)
            return out
        sites = records(SITES, 0x62dbcc)
        starts = records(STARTS, 0x62dbdc)

        strike = rng.choice(list(range(21, 50, 2)) + list(range(51, 80, 2)) + [97])
        backup = strike + 1
        vtol = rng.randrange(81, 96)
        slot = {'strike': strike, 'backup': backup, 'vtol': vtol}[routine]
        K = rng.randrange(1, 200)
        # The squad the units are in; the routine's own squads get most of them.
        squads = [strike, strike, strike, backup, backup, vtol, vtol, 1, 1, 2, 3, 0]
        nown = rng.randrange(1, 16)
        nen = rng.randrange(0, 6)
        total = nown + nen
        cx, cz = rng.randrange(500, 3000), rng.randrange(500, 3000)
        spread = rng.choice([40, 300, 900, 2500])
        put(PLAYER + 0x74, UNITS + USTRIDE); put(PLAYER + 0x78, UNITS + nown * USTRIDE)
        put(GAME + 0x14e84, UNITS); put(GAME + 0x14e88, UNITS + total * USTRIDE)
        members = {s: [] for s in range(1, 99)}
        units, classes = [], {}
        dying_army = False
        for i in range(1, total + 1):
            u, t, wp = UNITS + i * USTRIDE, TYPES + i * TSTRIDE, WEAPONS + i * 0x100
            own = i <= nown
            sq = rng.choice(squads) if own else 0
            flags = 0x1000000 if rng.random() < 0.95 else 0
            if rng.random() < 0.05: flags |= 0x1000
            if rng.random() < 0.85: flags |= 0x20
            if own and sq in (1, 2, 3) and rng.random() < 0.4: flags |= 0x2000000
            if rng.random() < 0.8: flags |= 0x8000000
            flags |= rng.choice([1, 1, 1, 2, 0])
            if not flags & 0x1000000 and own: sq = 0
            if flags & 0x1000 and sq in (strike, backup): dying_army = True
            maxd = rng.choice([0, 1, 100, 1000, 3999, 4000, 30000, 0xfffffff0])
            hp = rng.choice([maxd // 3, maxd // 4, maxd // 5, maxd // 3 - 1, maxd // 5 - 1, 3 * maxd // 4,
                             3 * maxd // 4 - 1, maxd, -1, rng.randrange(-32768, 32768)])
            hp = max(-32768, min(32767, hp))
            sight = rng.choice([0, 100, 300, 600, -5, 2000])
            canfly = rng.random() < 0.2
            tflags = (0x800 if canfly else 0) | (0x80000 if rng.random() < 0.15 else 0) | \
                     (0x100 if rng.random() < 0.1 else 0) | (0x10000 if flags & 0x8000000 else 0)
            secondary = 0x40000 if rng.random() < 0.05 else 0
            menu = rng.choice([0] * 9 + [1])
            depth = rng.choice([-10000, 0, 5])
            rang = rng.choice([0, 1, 15, 16, 200, 450, 900, -20])
            dmg, reload = rng.choice([0, 5, 30, 300]), rng.choice([0, 0, 1, 30, 90])
            mover = rng.random() < 0.92
            nav = rng.random() < 0.75
            far = 0 if own or rng.random() < 0.4 else rng.choice([3000, 8000])
            x = ((cx + far + rng.randrange(-spread, spread + 1)) << 16) | rng.randrange(0x10000)
            z = ((cz + far + rng.randrange(-spread, spread + 1)) << 16) | rng.randrange(0x10000)
            y = rng.randrange(0, 200 << 16)
            cellx, cellz = rng.randrange(0, 300), rng.randrange(0, 300)
            fx, fz = rng.choice([1, 2, 3]), rng.choice([1, 2, 3])
            has = own and rng.random() < 0.65
            mkind = rng.choice([K, K, K, (K + 1) & 0xff]) if has else 0
            mflags = rng.choice(FLAGS) if has else 0
            mclass = rng.choice([0, 1, 2, 7] + [14] * 12) if has else 0
            gx = (cx + rng.randrange(-200, 200)) << 16 if rng.random() < 0.5 else rng.getrandbits(32)
            gz = (cz + rng.randrange(-200, 200)) << 16 if rng.random() < 0.5 else rng.getrandbits(32)
            put(u + 0x130, flags); uc.mem_write(u + 2, struct.pack('<H', i))
            put(u + 0x68, x); put(u + 0x6c, y); put(u + 0x70, z)
            uc.mem_write(u + 0x74, struct.pack('<hhhh', cellx, cellz, fx, fz))
            uc.mem_write(u + 0x10c, struct.pack('<h', hp))
            put(u + 0xb4, t); put(u + 0xb8, PLAYER); put(u + 0xc8, sq); put(u + 0xc, wp)
            put(u + 8, MOVERS + i * 0x10 if mover else 0); put(MOVERS + i * 0x10 + 4, NAVS + i * 0x10 if nav else 0)
            put(t + 0x1be, maxd); uc.mem_write(t + 0x226, struct.pack('<h', sight))
            put(t + 0x260, tflags); put(t + 0x264, secondary); put(t + 0x12e, menu)
            uc.mem_write(t + 0x194, struct.pack('<h', depth))
            put(wp + 0x90, rang); uc.mem_write(wp + 0x88, struct.pack('<H', dmg)); uc.mem_write(wp + 0x9c, struct.pack('<H', reload))
            if has:
                m = MISSIONS + i * 0x80
                uc.mem_write(m + 4, bytes([mkind])); put(m + 0x5a, mflags); put(m + 0x22, gx); put(m + 0x2a, gz)
                put(u + 0x60, m)
                classes[i] = mclass
            if sq: members[sq].append(i)
            units.append([i, int(own), sq, flags, s32(x), s32(y), s32(z), cellx, cellz, fx, fz, hp, maxd, sight,
                          tflags, secondary, menu, depth, rang, dmg, reload, int(mover), int(nav), int(has), mkind,
                          mflags, mclass, s32(gx), s32(gz)])
        case['classes'] = [classes.get(i, 0) for i in range(total + 2)]
        for s in range(1, 99):
            rng.shuffle(members[s])
            d = DATA + s * DSTRIDE
            begin = VEC + s * CAP * 4
            for k, i in enumerate(members[s]): put(begin + 4 * k, UNITS + i * USTRIDE)
            put(d + 0xb8, begin); put(d + 0xbc, begin + 4 * len(members[s])); put(d + 0xc0, begin + 4 * CAP)
        anchors = []
        for b in range(1, 21):
            a = 0 if rng.random() < 0.6 else rng.randrange(-4, 5)
            put(DATA + b * DSTRIDE + 0xc, a); anchors.append(a)
        # The strike's state.
        sp = DATA + strike * DSTRIDE
        p0 = backup if rng.random() < 0.95 else strike
        p1 = rng.choice([200000, 500000, 1000000, rng.randrange(1, 300000)])
        p2 = rng.choice([-1, -1, -1, -1, 0, 1, 2, 3, 5, 10])
        if dying_army: p2 = -1   # retail's dissolve loop would spin on a dying member
        tgt = rng.choice([0, total + 3] + list(range(1, total + 1)) + list(range(nown + 1, total + 1)) * 3)
        p5 = rng.choice([0, 1, 2, 2])
        # Some goals on or near the target (the march's 50 px "close" test).
        if 1 <= tgt <= total:
            tx, tz = get(UNITS + tgt * USTRIDE + 0x68), get(UNITS + tgt * USTRIDE + 0x70)
            for u in units:
                if u[23] and rng.random() < 0.3:
                    gx = (tx + (rng.randrange(-60, 61) << 16)) & 0xffffffff
                    gz = (tz + (rng.randrange(-60, 61) << 16)) & 0xffffffff
                    put(MISSIONS + u[0] * 0x80 + 0x22, gx); put(MISSIONS + u[0] * 0x80 + 0x2a, gz)
                    u[27], u[28] = s32(gx), s32(gz)
        for k, v in enumerate([p0, p1, p2, rng.randrange(20), tgt, p5]): put(sp + 0xc + 4 * k, v)
        put(DATA + backup * DSTRIDE + 0xc, strike)
        active, dirty = rng.randrange(2), rng.randrange(2)
        put(DATA + slot * DSTRIDE + 8, active); put(DATA + slot * DSTRIDE + 0xb0, dirty)
        # The visible-enemy list.
        listed = [i for i in range(1, total + 1) if rng.random() < 0.3]
        for k, i in enumerate(listed): put(LIST + 4 * k, UNITS + i * USTRIDE)
        put(BM + 9, LIST); put(BM + 0xd, LIST + 4 * len(listed))
        pickans = rng.choice([0] + list(range(1, total + 1)) + list(range(nown + 1, total + 1)) * 4)
        kinds = [rng.choice([K] * 8 + [0, (K + 1) & 0xff]) for _ in range(80)]
        reachable = [int(rng.random() < 0.7) for _ in range(40)]
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        case.update(pick=pickans, kinds=kinds, resolved=0, reach=reachable, reached=0)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(THINK[routine], ecx=SQOBJ + slot * 0x20)
        if error: raise RuntimeError(error)
        row = [get(0x64186c), get(SQOBJ + slot * 0x20 + 0xc), len(log)] + [v for e in log for v in e]
        row += [geti(UNITS + i * USTRIDE + 0xc8) for i in range(1, nown + 1)]
        for s in range(1, 99):
            d = DATA + s * DSTRIDE
            b, e = get(d + 0xb8), get(d + 0xbc)
            ids = [index(get(a)) for a in range(b, e, 4)] if b else []
            row += [len(ids)] + ids
        for s in (strike, backup, vtol):
            d = DATA + s * DSTRIDE
            row += [geti(d + 0xc + 4 * k) for k in range(9)] + [get(d + 8), get(d + 0xb0)]
        row += [geti(DATA + b * DSTRIDE + 0xc) for b in range(1, 21)]
        expected.append(row)
        orders = [e for e in log if e[0] == 3]
        stats['orders'] += len(orders)
        stats['clears'] += sum(1 for e in log if e[0] == 4)
        stats['picks'] += sum(1 for e in log if e[0] == 6)
        stats['reach'] += sum(1 for e in log if e[0] == 5)
        state = geti(sp + 0x20)
        if routine == 'strike':
            stats['contact'] += state == 1
            stats['march'] += state == 2
            stats['none'] += state == 0
        line = [tick, w, h, side, seed, strike, vtol, len(sites)] + [v for s in sites for v in s]
        line += [len(starts)] + [v for s in starts for v in s] + anchors
        line += [nown, nen] + [v for u in units for v in u]
        for s in range(1, 99): line += [len(members[s])] + members[s]
        line += [p0, p1, p2, get(sp + 0x18), tgt, p5, active, dirty]
        line += [len(listed)] + listed + [pickans, len(kinds)] + kinds + [len(reachable)] + reachable
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([binary, '--' + routine], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        diff = [(k, a, b) for k, (a, b) in enumerate(zip(actual[i], expected[i])) if a != b][:6]
        raise AssertionError('%d of %d %s thinks differ; first: case %d, (index, port, retail) %s\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), routine, i, diff, actual[i][:80], expected[i][:80], rows[i][:2500]))
    stats['paths'] = ', '.join('%s %d' % kv for kv in sorted(hits.items()))
    return len(rows), stats


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--routine', choices=('strike', 'backup', 'vtol'), default='strike')
    args = ap.parse_args()
    n, st = run(args.routine, args.binary, args.cases)
    name = {'strike': '0x40e180 strike', 'backup': '0x40ea30 backup', 'vtol': '0x40eb40 VTOL'}[args.routine]
    extra = (', end states: %d contact, %d march, %d none' % (st['contact'], st['march'], st['none'])
             if args.routine == 'strike' else '')
    print('PASS: %d %s thinks, 0 mismatches: %d orders, %d mission clears, %d target picks, %d reachability '
          'queries%s; retail paths reached: %s' % (n, name, st['orders'], st['clears'], st['picks'], st['reach'], extra,
                                                  st['paths']))


if __name__ == '__main__':
    main()
