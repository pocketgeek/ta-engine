#!/usr/bin/env python3
"""Compare the Classic brain's wave launch with retail's.

Per case it runs retail's whole base think 0x40b320 for one base squad of a
random player, with its three member planners stubbed, so what is checked is
the think's own work: the clock, the stale-anchor rule, the centre and radius
gate, the member loop's dispatch, and its tail -- the quota growth, the ready
counts, the transfer and expansion calls, the launch into a strike's backup, a
VTOL squad or a naval strike and the wave re-rolls. Each case has up to 24
units in the base (ground, air and naval combat units, builders free or busy,
structures, the Monarch; live, dying, selectable, finished or not, held,
carried; hp around 3/4), the base's quotas and wave sizes (positive and
negative), its anchor, the owner's population, ten player records for the
ground roll's E, and random strike, backup, naval and VTOL slots already in use.

Everything is retail's own code -- the ready count 0x40a7b0, the member mover
0x40b8e0, the first-empty searches 0x40af20 / 0x40deb0 / 0x40df20 / 0x40eaf0,
the wave rolls 0x40c240 / 0x40c3c0 / 0x40c490, set-squad 0x50b4f0, set-active
0x50b700, the population 0x410e40, the categories, centre, anchors and radius --
except, logged instead of run: the factory, constructor and combat planners
0x40dcb0 / 0x40c890 / 0x40d370 (each has its own oracle), the threat test
0x412000 (answers no), the mission clear 0x4d6a50, the mission-descriptor
lookup 0x4d4cc0 (answers the case's class), and the four P7 routines the tail
calls: the transfer targets 0x40b050 / 0x40b140 / 0x40b230 and the expansion
0x40bc20 (they answer from the case) and the transfer 0x40b9e0.

The port (classic_ai_test --launch: the shared sim::retailTickAiSquad clock
template, ai::classicBaseMembers and ai::classicBaseLaunch) must make the same
draws and calls in the same order and leave the same seed, deadline, unit
squads, member lists and squad parameters.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_launch.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_FPCW

GAME = HEAP                  # +0x2404 player records, +0x19f44 tick
MGR = HEAP + 0x22400
BM = HEAP + 0x22600          # build manager: +0x7d population
SQOBJ = HEAP + 0x23000
DATA = HEAP + 0x24000
VEC = HEAP + 0x29000
UNITS = HEAP + 0x30000
TYPES = HEAP + 0x40000
MISSIONS = HEAP + 0x4c000
MOVERS = HEAP + 0x50000
HOLDER, NONHOLDER = HEAP + 0x51000, HEAP + 0x51200
DESC = HEAP + 0x52000
STARTS, SITES = HEAP + 0x53000, HEAP + 0x54000
ARENA = HEAP + 0x100000
USTRIDE, TSTRIDE, DSTRIDE, CAP, RSTRIDE = 0x138, 0x2a4, 0xc4, 64, 0x110
VTABLE = {'base': 0x5ebb2c, 'strike': 0x5ebb50, 'backup': 0x5ebb60, 'vtol': 0x5ebb70, 'reserve': 0x5ebb1c}


def kind(slot):
    if slot <= 20: return 'base'
    if slot <= 80: return 'strike' if slot % 2 else 'backup'
    if slot <= 95: return 'vtol'
    return {96: 'reserve', 97: 'strike', 98: 'backup'}[slot]


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
    geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
    heap = [ARENA]
    log = []
    case = {}

    def index(address):
        return (address - UNITS) // USTRIDE if address else 0
    def new(uc_, argv):
        size = get(argv); at = heap[0]; heap[0] = (at + size + 15) & ~15
        return 0, at
    def planner(which, nargs):
        def hook(uc_, argv):
            log.append((7, which, index(get(argv)))); return nargs, 0
        return hook
    def clear(uc_, argv):
        u, arg = struct.unpack('<II', uc_.mem_read(argv, 8))
        log.append((4, index(u), arg)); return 2, 0
    def descriptor(uc_, argv):
        m = uc_.reg_read(UC_X86_REG_ECX) - 4
        return 0, DESC + case['classes'][(m - MISSIONS) // 0x80] * 0x20
    def lookup(which):
        def hook(uc_, argv):
            log.append((8, which)); return 0, case['bases'][which]
        return hook
    def transfer(uc_, argv):
        a = struct.unpack('<4i', uc_.mem_read(argv, 16))
        log.append((9,) + a); return 4, 0
    def expand(uc_, argv):
        a = struct.unpack('<2i', uc_.mem_read(argv, 8))
        log.append((10,) + a); return 2, case['expand']
    p.hooks.update({0x4eb9e0: new, 0x4eba00: lambda uc_, argv: (0, 0),
                    0x40dcb0: planner(0, 1), 0x40c890: planner(1, 2), 0x40d370: planner(2, 2),
                    0x412000: lambda uc_, argv: (3, 0), 0x4d6a50: clear, 0x4d4cc0: descriptor,
                    0x40b050: lookup(0), 0x40b140: lookup(1), 0x40b230: lookup(2),
                    0x40b9e0: transfer, 0x40bc20: expand})
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: log.append(
        (0, s32(get(uc_.reg_read(UC_X86_REG_ESP) + 4)))), begin=0x535cc0, end=0x535cc0)
    for c in range(32):
        uc.mem_write(DESC + c * 0x20, bytes(0x10) + bytes([c]) + bytes(0xf))
    hits = {}
    def count(name):
        def hook(uc_, a, size, d): hits[name] = hits.get(name, 0) + 1
        return hook
    for address, name in ((0x40b500, 'quota growth'), (0x40b64d, 'builder transfer draw'),
                          (0x40b691, 'expansion'), (0x40b6a5, 'escort ground'), (0x40b6c8, 'escort air'),
                          (0x40b6f9, 'ground transfer'), (0x40b72c, 'air transfer'), (0x40b776, 'strike taken'),
                          (0x40b792, 'ground launch'), (0x40b7fd, 'VTOL launch'), (0x40b83c, 'air into backup'),
                          (0x40b895, 'naval launch'), (0x40b8d9, 'end')):
        uc.hook_add(UC_HOOK_CODE, count(name), begin=address, end=address)

    rng = random.Random(0x40b4cf)
    rows, expected = [], []
    put(0x62d55c, GAME)
    for n in range(args.cases):
        heap[0] = ARENA; log.clear()
        uc.mem_write(GAME, bytes(0x20000)); uc.mem_write(MGR, bytes(0x1b0)); uc.mem_write(BM, bytes(0x100))
        for base, size in ((UNITS, 0x4000), (TYPES, 0x8000), (MISSIONS, 0x2000), (MOVERS, 0x400)):
            uc.mem_write(base, bytes(size))
        put(HOLDER + 0x130, 0x4000000 | 0x1000000); put(NONHOLDER + 0x130, 0x1000000)
        tick = rng.choice([0, 1000, 54321, 0xfffffff0, rng.getrandbits(32)])
        put(GAME + 0x19f44, tick)
        own = rng.randrange(10)
        PLAYER = GAME + 0x2404 + own * RSTRIDE
        records = []
        allied = [int(rng.random() < 0.3) for _ in range(12)]
        allied[own] = 1
        for r in range(10):
            rec = GAME + 0x2404 + r * RSTRIDE
            active = int(rng.random() < 0.85) if r != own else 1
            kd = rng.choice([1, 1, 2, 2, 3, 0, 4]) if r != own else 2
            idx = rng.choice([r, r, r, 10, rng.randrange(12)]) if r != own else own
            pop = rng.choice([0, 5, 9, 10, 11, 40, 125, 300, rng.randrange(0x10000)])
            put(rec, active); uc.mem_write(rec + 0xea, bytes([kd, idx]))
            uc.mem_write(rec + 0xe8, struct.pack('<H', pop))
            records.append([active, kd, idx, pop])
        uc.mem_write(PLAYER + 0xac, bytes(allied))
        side = own
        for s in range(10): put(0x62a33c + 4 * s, BM if s == side else 0)
        population = rng.choice([0, 19, 20, 21, 39, 40, 41, 60, 61, 90, 300, rng.randrange(1000)])
        put(BM + 0x7d, population)
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

        def anchors(base, pointer):
            out = []
            for k in range(rng.choice([0, 1, 2, 4])):
                valid = int(rng.random() < 0.9)
                x, z = rng.randrange(0, 200), rng.randrange(0, 200)
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((valid, x, z))
            put(pointer, base); put(pointer + 4, base + len(out) * 101)
            return out
        sites = anchors(SITES, 0x62dbcc)
        starts = anchors(STARTS, 0x62dbdc)

        slot = rng.choice([1, 1, 1, 2, 3, rng.randrange(1, 21)])
        d = DATA + slot * DSTRIDE
        params = [0 if rng.random() < 0.6 else rng.randrange(-4, 5)]
        params += [rng.randrange(0, 4), rng.randrange(0, 6), rng.randrange(0, 4), rng.randrange(0, 4)]
        params += [rng.choice([1, 1, 2, 3, 5, 8]) * rng.choice([1, -1]) for _ in range(3)]
        params += [rng.randrange(5)]
        for k, v in enumerate(params): put(d + 0xc + 4 * k, v)
        nunits = rng.choice([0, 1, 3, 6, 10, 16, 24])
        put(PLAYER + 0x74, UNITS + USTRIDE); put(PLAYER + 0x78, UNITS + nunits * USTRIDE)
        cx, cz = rng.randrange(500, 3000), rng.randrange(500, 3000)
        units, classes = [], {}
        roles = ['ground'] * 6 + ['air'] * 2 + ['naval'] * 2 + ['builder'] * 2 + ['structure', 'monarch', 'other']
        for i in range(1, nunits + 1):
            u, t = UNITS + i * USTRIDE, TYPES + i * TSTRIDE
            role = rng.choice(roles)
            flags = 0x1000000 if rng.random() < 0.97 else 0
            if rng.random() < 0.04: flags |= 0x1000
            if rng.random() < 0.9: flags |= 0x20
            if role == 'structure': flags |= 0x2000000
            if role in ('ground', 'air', 'naval', 'monarch') or rng.random() < 0.2: flags |= 0x8000000
            flags |= 1
            tflags = (0x800 if role == 'air' else 0) | (0x80000 if role == 'naval' else 0) | \
                     (0x100 if role == 'builder' else 0)
            secondary = 0x40000 if role == 'monarch' else 0
            menu = 1 if role in ('builder', 'monarch') or (role == 'structure' and rng.random() < 0.5) else 0
            depth = rng.choice([5, 12, 0]) if role == 'naval' else rng.choice([-10000, -10000, 0])
            mover = int(role != 'structure' and rng.random() < 0.95)
            maxd = rng.choice([100, 1000, 4000, 0, 0xfffffff0])
            hp = rng.choice([maxd, maxd, 3 * maxd // 4, 3 * maxd // 4 - 1, maxd // 2, -1])
            hp = max(-32768, min(32767, hp))
            f108 = rng.choice([0.0] * 8 + [0.5, float('nan'), -0.0])
            held = 0 if rng.random() < 0.95 else 1
            carrier = rng.choice([0] * 12 + [1, 2])
            has = rng.random() < 0.5
            mflags = rng.choice([0x3000400, 0x80508, 0x2000, 0x8010c, 0]) if has else 0
            mclass = rng.choice([0, 1, 2, 7, 14, 19]) if has else 0
            x = ((cx + rng.randrange(-300, 301)) << 16) | rng.randrange(0x10000)
            z = ((cz + rng.randrange(-300, 301)) << 16) | rng.randrange(0x10000)
            fx, fz = rng.choice([1, 2, 3, 4]), rng.choice([1, 2, 3, 4])
            put(u + 0x130, flags); uc.mem_write(u + 2, struct.pack('<H', i))
            put(u + 0x68, x); put(u + 0x70, z)
            uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz))
            uc.mem_write(u + 0x10c, struct.pack('<h', hp))
            uc.mem_write(u + 0x108, struct.pack('<f', f108)); put(u + 0x104, held)
            put(u + 0xa8, [0, HOLDER, NONHOLDER][carrier])
            put(u + 0xb4, t); put(u + 0xb8, PLAYER); put(u + 0xc8, slot if flags & 0x1000000 else 0)
            put(u + 8, MOVERS + i * 0x10 if mover else 0)
            put(t + 0x1be, maxd); put(t + 0x260, tflags); put(t + 0x264, secondary); put(t + 0x12e, menu)
            uc.mem_write(t + 0x194, struct.pack('<h', depth))
            if has:
                m = MISSIONS + i * 0x80
                put(m + 0x5a, mflags); put(u + 0x60, m)
                classes[i] = mclass
            f = struct.unpack('<I', struct.pack('<f', f108))[0]
            units.append([i, flags, s32(x), s32(z), fx, fz, hp, maxd, tflags, secondary, menu, depth, mover,
                          f, held, carrier, int(has), mflags, mclass])
        case['classes'] = [classes.get(i, 0) for i in range(nunits + 2)]
        members = {s: [] for s in range(1, 99)}
        members[slot] = [u[0] for u in units if u[1] & 0x1000000]
        rng.shuffle(members[slot])
        # Other squads already in use (dummy members: unit slot 0).
        for s in list(range(21, 81)) + list(range(81, 96)) + [97, 98]:
            if rng.random() < rng.choice([0.0, 0.3, 0.9]): members[s] = [0] * rng.randrange(1, 3)
        for s in range(1, 99):
            dd = DATA + s * DSTRIDE
            begin = VEC + s * CAP * 4
            for k, i in enumerate(members[s]): put(begin + 4 * k, UNITS + i * USTRIDE)
            put(dd + 0xb8, begin); put(dd + 0xbc, begin + 4 * len(members[s])); put(dd + 0xc0, begin + 4 * CAP)
        bases = [rng.choice([0, 0, 0, rng.randrange(1, 21)]) for _ in range(3)]
        expand = int(rng.random() < 0.3)
        seed = rng.getrandbits(31) | 1
        put(0x64186c, seed)
        case.update(bases=bases, expand=expand)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x40b320, ecx=SQOBJ + slot * 0x20)
        if error: raise RuntimeError(error)
        row = [get(0x64186c), get(SQOBJ + slot * 0x20 + 0xc), len(log)] + [v for e in log for v in e]
        row += [geti(UNITS + i * USTRIDE + 0xc8) for i in range(1, nunits + 1)]
        for s in range(1, 99):
            dd = DATA + s * DSTRIDE
            b, e = get(dd + 0xb8), get(dd + 0xbc)
            ids = [index(get(a)) for a in range(b, e, 4)] if b else []
            row += [len(ids)] + ids + [geti(dd + 0xc + 4 * k) for k in range(9)] + [get(dd + 8), get(dd + 0xb0)]
        expected.append(row)
        line = [tick, own, seed, slot, population] + allied + [v for r in records for v in r]
        line += [len(sites)] + [v for s in sites for v in s] + [len(starts)] + [v for s in starts for v in s]
        line += params + [nunits] + [v for u in units for v in u]
        for s in range(1, 99): line += [len(members[s])] + members[s]
        line += bases + [expand]
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--launch'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        diff = [(k, a, b) for k, (a, b) in enumerate(zip(actual[i], expected[i])) if a != b][:6]
        raise AssertionError('%d of %d base thinks differ; first: case %d, (index, port, retail) %s\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, diff, actual[i][:80], expected[i][:80], rows[i][:2500]))
    print('PASS: %d 0x40b320 base thinks (planners stubbed), 0 mismatches; retail paths reached: %s'
          % (len(rows), ', '.join('%s %d' % kv for kv in sorted(hits.items()))))


if __name__ == '__main__':
    main()
