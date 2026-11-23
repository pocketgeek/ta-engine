#!/usr/bin/env python3
"""Compare the Classic brain's base think and factory planner with retail's.

Per case it runs, as one tick of a retail AI player would, the economy step's
allocation block 0x51d837..0x51d8c4 on a random store, income, storage and
construction demand, and then a base squad's think 0x40b320 from its start to
the end of its member loop (0x40b4cf). Everything the factory path touches is
retail's own code: the reschedule draw, the empty-base and stale-anchor rules,
the base centre 0x40c610 (anchor 0x40c750, centroid 0x40aa50, categories
0x40a5f0), the ring radius 0x40cf30, the member filter, the factory planner
0x40dcb0, the menu draw 0x412d00, the entry weight 0x412c90 and the limit check
0x409890 -> 0x410de0 over a random build manager (pref, priority, limit and count
tables, limits on or off, player indices around retail's 10) and a random type
table (menus with repeats, lodestone-class yardmaps, side strings that differ
only in case, costs around the 500 deferral line, commanders). The planner reads
the allocation the economy block left in the owner's resource object, so this
also settles which quantity its 0.7 and 0.7/3 gates read (plan unknown U4).

Logged instead of run: the threat test 0x412000 (it returns the case's random
answer), the constructor and combat planners 0x40c890 / 0x40d370 (later phases;
the oracle checks which members reach them, and with which threat), the
mission-name lookup 0x4d4bf0 (BuildingBuild is mission kind 3 here) and the
queue call 0x429810 (type, factory, count). The mission descriptor table that
0x4d4cc0 indexes is a small fake one, so a member's first-mission class is the
case's own.

The port (classic_ai_test --factory-think: sim::RetailConstructionResources for
the allocation, then sim::retailTickAiSquad -> ai::classicBaseMembers ->
ai::classicPlanFactory, as ClassicBrain runs them) must leave the same deadline,
anchor and RNG seed, make the same draws in the same order, and make the same
planner and queue calls in the same order. The FPU runs at the C runtime's
53-bit precision (0x027f), as in check_ai_profile.py.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_factory_think.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_FPCW, UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EBP,
                               UC_X86_REG_EDX, UC_X86_REG_EDI)

GAME = HEAP                  # game state: +0x19f44 tick, +0x175b8 type count, +0x175c4 type table
PLAYER = HEAP + 0x22000      # +0x10c resource object, +0xe8 unit count
RES = HEAP + 0x22400
MGR = HEAP + 0x22600         # AI manager: [0] player, +4 player index
SQOBJ = HEAP + 0x22800       # the base squad object
DATA = HEAP + 0x22900        # its data record: +0xc anchor, +0xb8.. member vector
VEC = HEAP + 0x22a00
BM = HEAP + 0x23000          # build manager: +0x69 priority, +0x85 count, +0xc5 pref, +0xe5 limit
TABLES = HEAP + 0x23100
UNITS = HEAP + 0x24000
TYPES = HEAP + 0x28000       # 676-byte type records, index 0 the sentinel
MENUS = HEAP + 0x30000
STRINGS = HEAP + 0x31000
MISSIONS = HEAP + 0x32000
MTAB = HEAP + 0x34000        # fake mission descriptor table (25-byte entries, class at +0x10)
HOLDER, NONHOLDER = HEAP + 0x38000, HEAP + 0x38200
NAV = HEAP + 0x39000
STARTS, SITES = HEAP + 0x3a000, HEAP + 0x3b000
USTRIDE, TSTRIDE = 0x138, 676
BUILDINGBUILD = 3
MISSION_CLASS = [0, 1, 2, 0, 1, 2, 7, 0]       # descriptor class byte per mission kind
SIDES = [b'Aramon\0', b'Veruna\0', b'aramon\0']
NOISE = [0x1, 0x2, 0x8, 0x40, 0x100, 0x400, 0x2000, 0x10000, 0x80000, 0x4000000, 0x10000000]


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
    geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
    log, draws = [], []
    threat = [0]

    def unit_index(address):
        return (address - UNITS) // USTRIDE
    def threat_test(uc_, argv):
        log.append((0, 0, 0)); return 3, threat[0]
    def constructor(uc_, argv):
        u, t = struct.unpack('<II', uc_.mem_read(argv, 8))
        log.append((2, unit_index(u), t & 0xff)); return 2, 0
    def combat(uc_, argv):
        u, t = struct.unpack('<II', uc_.mem_read(argv, 8))
        log.append((3, unit_index(u), t & 0xff)); return 2, 0
    def identify(uc_, argv):
        if get(argv) != 0x605064: raise AssertionError('unexpected mission name %#x' % get(argv))
        uc_.mem_write(uc_.reg_read(UC_X86_REG_ECX), bytes([BUILDINGBUILD])); return 1, uc_.reg_read(UC_X86_REG_ECX)
    def queue(uc_, argv):
        name, u, count = struct.unpack('<III', uc_.mem_read(argv, 12))
        if count != 1 or (name - TYPES - 0x20) % TSTRIDE: raise AssertionError('queue call %#x %d' % (name, count))
        log.append((1, unit_index(u), (name - TYPES - 0x20) // TSTRIDE)); return 3, 0
    p.hooks[0x412000] = threat_test
    p.hooks[0x40c890] = constructor
    p.hooks[0x40d370] = combat
    p.hooks[0x4d4bf0] = identify
    p.hooks[0x429810] = queue
    p.freeze_hooks()
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: draws.append(
        struct.unpack('<i', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP) + 4, 4))[0]), begin=0x535cc0, end=0x535cc0)
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: uc_.emu_stop(), begin=0x40b4cf, end=0x40b4cf)
    planned = [0]
    def factory_entry(uc_, a, s, d): planned[0] += 1
    uc.hook_add(UC_HOOK_CODE, factory_entry, begin=0x40dcb0, end=0x40dcb0)

    rng = random.Random(0x40dcb0 ^ 0x40b320)
    rows, expected = [], []
    stats = {'factory': 0, 'queued': 0, 'constructor': 0, 'combat': 0, 'low': 0, 'mid': 0, 'deferral': 0}
    for case in range(args.cases):
        log.clear(); draws.clear()
        uc.mem_write(GAME, bytes(0x1a000)); uc.mem_write(PLAYER, bytes(0x200)); uc.mem_write(RES, bytes(0x40))
        uc.mem_write(MGR, bytes(0x1b0)); uc.mem_write(DATA, bytes(0xc4)); uc.mem_write(BM, bytes(0x100))
        uc.mem_write(UNITS, bytes(0x4000)); uc.mem_write(TYPES, bytes(0x8000)); uc.mem_write(MISSIONS, bytes(0x2000))
        uc.mem_write(HOLDER, bytes(0x400)); put(HOLDER + 0x130, 0x4000000 | 0x1000000)
        put(0x62d55c, GAME)
        tick = rng.choice([0, 30, 31, 1000, rng.randrange(1, 1 << 20)])
        put(GAME + 0x19f44, tick)

        # The owner, its economy and the AI player index.
        index = rng.choice([0, 1, 2, 5, 9, 9, 10, 11])
        limits = rng.random() < 0.8
        if index < 10: uc.mem_write(GAME + index * 0x110 + 0x24e7, bytes([int(limits)]))
        limited = index < 10 and limits
        put(0x62a33c + index * 4, BM)
        put(PLAYER + 0x10c, RES)
        population = rng.choice([0, 5, 29, 30, 31, 45, 59, 60, 61, 200, rng.randrange(65536)])
        uc.mem_write(PLAYER + 0xe8, struct.pack('<H', population))
        stored = f32(rng.choice([0, 1, 10, 100, 500, rng.uniform(0, 5000)]))
        override = f32(rng.choice([0, 0, 0, 100]))
        produced, total, excess = f32(rng.random() * 100), rng.random() * 1e6, rng.random() * 1e4
        income = f32(rng.choice([0, 31, 47, rng.random() * 300]))
        storage = f32(rng.choice([0, 1000, rng.random() * 5000]))
        # Demand per second around the allocation lines: stored / (demand / 30)
        # near 1, 0.7 and 0.7/3, or none (allocation 1).
        line = rng.choice([1, 0.7, 0.7, 0.7 / 3, 0.2, 0.3, 0.5, 0.5, 0.6, 3])
        after = stored + income * 0.03333333
        demand = f32(rng.choice([0, max(after, 1) * 30 / line * rng.choice([0.999, 1, 1.001]), rng.random() * 30000]))
        uc.mem_write(RES, struct.pack('<f', stored)); uc.mem_write(RES + 0x14, struct.pack('<f', override))
        uc.mem_write(RES + 0xc, struct.pack('<f', produced)); uc.mem_write(RES + 0x18, struct.pack('<d', total))
        uc.mem_write(RES + 0x20, struct.pack('<d', excess))
        frame = STACK + STACK_SZ - 0x2000
        uc.mem_write(frame - 8, struct.pack('<f', income)); uc.mem_write(frame - 12, struct.pack('<f', storage))
        uc.mem_write(frame - 24, struct.pack('<f', demand))
        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        uc.reg_write(UC_X86_REG_EBP, frame); uc.reg_write(UC_X86_REG_EDX, PLAYER)
        uc.reg_write(UC_X86_REG_EDI, 0x3f800000)
        uc.emu_start(0x51d837, 0x51d8c4, timeout=5_000_000)

        # Types: tables of the build manager, menus, sides, yardmaps, costs.
        ntypes = rng.randrange(2, 14)
        put(GAME + 0x175b8, ntypes); put(GAME + 0x175c4, TYPES)
        uc.mem_write(STRINGS, b''.join(SIDES)); uc.mem_write(STRINGS + 0x40, bytes([0x8f, 0x0f, 0x80]))
        prio_t, count_t, pref_t, limit_t = TABLES, TABLES + 0x40, TABLES + 0x80, TABLES + 0xc0
        put(BM + 0x69, prio_t); put(BM + 0x85, count_t); put(BM + 0xc5, pref_t); put(BM + 0xe5, limit_t)
        types = []
        for t in range(ntypes):
            rec = TYPES + t * TSTRIDE
            pref = rng.choice([0, 1, 50, 99, 100, 100, 100, 100, 255])
            prio = rng.choice([-100, -1, 0, 1, 21, 42, 84, 84, 100, 127])
            limit = rng.choice([-1, -1, 0, 1, 2, 4, rng.randrange(-5, 40)])
            count = rng.choice([0, 0, 1, 2, 3, 5, -1, 40])
            lode = rng.choice([0, 0, 1, 2, 3])          # no yardmap, 0x8f, 0x0f, 0x80
            side = rng.choice([0, 0, 0, 1, 2])
            cost = f32(rng.choice([0, 100, 499.99, 500, 500.01, 800, 801, 2000, 2000, 6000, rng.uniform(0, 5000)]))
            commander = rng.random() < 0.15
            uc.mem_write(prio_t + t, struct.pack('<b', prio)); uc.mem_write(count_t + 2 * t, struct.pack('<h', count))
            uc.mem_write(pref_t + t, bytes([pref])); uc.mem_write(limit_t + 4 * t, struct.pack('<i', limit))
            put(rec + 0x8a, STRINGS + 7 * side)
            put(rec + 0x12a, 0 if lode == 0 else STRINGS + 0x40 + lode - 1)
            uc.mem_write(rec + 0x20e, struct.pack('<f', cost))
            put(rec + 0x264, 0x40000 if commander else 0)
            r = rng.random()
            if t == 0 or r < 0.3: menu = []
            else: menu = [rng.randrange(1, ntypes) for _ in range(rng.choice([1, 2, 3, 5, 8]))]
            count_field = -1 if not menu and rng.random() < 0.1 else len(menu)
            put(rec + 0x12e, count_field)
            for k, e in enumerate(menu): uc.mem_write(MENUS + t * 0x40 + 2 * k, struct.pack('<H', e))
            put(rec + 0x132, MENUS + t * 0x40)
            types.append([pref, prio, limit, count, int(lode == 1 or lode == 3), side, bits(cost), int(commander),
                          count_field, len(menu)] + menu)

        # The base squad, its anchor tables and members.
        slot = rng.choice([1, 1, 2, 7, 20])
        put(MGR, PLAYER); uc.mem_write(MGR + 4, bytes([index]))
        put(SQOBJ, 0x5ebb2c); put(SQOBJ + 4, MGR); put(SQOBJ + 8, DATA); put(SQOBJ + 12, 0); put(SQOBJ + 16, index)
        put(MGR + 0x11 + slot * 4, SQOBJ)

        def records(base, pointer):
            n = rng.choice([0, 1, 2, 3, 5])
            out = []
            for k in range(n):
                valid = rng.random() < 0.8
                x, z = rng.randrange(0, 160), rng.randrange(0, 160)
                uc.mem_write(base + k * 101, bytes([int(valid)]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((int(valid), x, z))
            put(pointer, base); put(pointer + 4, base + n * 101)
            return out
        starts = records(STARTS, 0x62dbdc)
        sites = records(SITES, 0x62dbcc)
        anchor = 0 if rng.random() < 0.5 else rng.randrange(-6, 7)
        put(DATA + 0xc, anchor)

        n = rng.choice([0, 1, 2, 3, 4, 6, 8, 12])
        put(PLAYER + 0x74, UNITS); put(PLAYER + 0x78, UNITS + max(n - 1, 0) * USTRIDE)
        units = []
        for i in range(n):
            u = UNITS + i * USTRIDE
            flags = 0x1000000 if rng.random() < 0.95 else 0
            if rng.random() < 0.08: flags |= 0x1000
            if rng.random() < 0.9: flags |= 0x20
            if rng.random() < 0.6: flags |= 0x2000000
            if rng.random() < 0.5: flags |= 0x8000000
            for bit in NOISE:
                if rng.random() < 0.05: flags |= bit
            t = rng.randrange(ntypes)
            remaining = rng.choice([0.0] * 6 + [-0.0, 0.5, float('nan')])
            held = 0 if rng.random() < 0.92 else 1
            carrier = rng.choice([0] * 12 + [1, 2])
            mover = rng.random() < 0.6
            x, z = rng.randrange(0, 2500 << 16), rng.randrange(0, 2500 << 16)
            fx, fz = rng.choice([1, 2, 3, 4, 6]), rng.choice([1, 2, 3, 4, 6])
            kinds = [rng.randrange(len(MISSION_CLASS)) for _ in range(rng.choice([0, 0, 0, 0, 1, 1, 2, 3]))]
            head = 0
            for k in reversed(range(len(kinds))):
                m = MISSIONS + (i * 4 + k) * 0x70
                uc.mem_write(m + 4, bytes([kinds[k]])); put(m + 0x66, head); head = m
            put(u + 0x60, head)
            put(u + 0x130, flags); put(u + 0xb4, TYPES + t * TSTRIDE); put(u + 0xb8, PLAYER); put(u + 0xc8, slot)
            uc.mem_write(u + 0x108, struct.pack('<f', remaining)); put(u + 0x104, held)
            put(u + 0xa8, [0, HOLDER, NONHOLDER][carrier]); put(u + 8, NAV if mover else 0)
            put(u + 0x68, x); put(u + 0x6c, 0); put(u + 0x70, z)
            uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz))
            producing = int(BUILDINGBUILD in kinds)
            first = MISSION_CLASS[kinds[0]] if kinds else 0
            units.append([flags, t, bits(remaining), held, carrier, int(mover), x, z, fx, fz, producing, first])
        uc.mem_write(MTAB, b''.join(bytes(16) + bytes([c]) + bytes(8) for c in MISSION_CLASS))
        put(0x62db84, MTAB)
        members = list(range(n))
        rng.shuffle(members)
        members = members[:rng.choice([len(members), len(members), max(len(members) - 1, 0)])]
        for k, i in enumerate(members): put(VEC + 4 * k, UNITS + i * USTRIDE)
        if members: put(DATA + 0xb8, VEC); put(DATA + 0xbc, VEC + 4 * len(members)); put(DATA + 0xc0, VEC + 0x100)
        threat[0] = rng.randrange(2)
        seed = rng.getrandbits(32)
        put(0x64186c, seed)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x40b320, ecx=SQOBJ, allow_early_stop=True)
        if error: raise RuntimeError(error)
        allocation = struct.unpack('<f', uc.mem_read(RES + 8, 4))[0]
        stats['factory'] += planned[0]
        if planned[0]:
            stats['low'] += allocation < 0.7 / 3
            stats['mid'] += 0.7 / 3 <= allocation < 0.7
            stats['deferral'] += 30 in draws[1:]
        planned[0] = 0
        for e in log:
            stats[{0: 'factory', 1: 'queued', 2: 'constructor', 3: 'combat'}[e[0]]] += e[0] != 0
        row = [get(SQOBJ + 12), geti(DATA + 0xc), get(0x64186c), len(draws), *draws, len(log)]
        row += [v for e in log for v in e]
        expected.append(row)
        line = [tick, slot, anchor, seed, int(limited), population, threat[0],
                bits(stored), bits(override), bits(produced), repr(total), repr(excess),
                bits(income), bits(storage), bits(demand), len(starts)] + [v for r in starts for v in r]
        line += [len(sites)] + [v for r in sites for v in r] + [ntypes] + [v for t in types for v in t]
        line += [n] + [v for u in units for v in u] + [len(members)] + members
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--factory-think'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        raise AssertionError('%d of %d base thinks differ; first: case %d\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, actual[i], expected[i], rows[i]))
    print('PASS: %d economy steps + base thinks, 0 mismatches: %d factory plans (%d under the 0.7/3 gate, %d '
          'between it and 0.7, %d with a cost-deferral draw), %d units queued, %d constructor and %d combat '
          'dispatches' % (len(rows), stats['factory'], stats['low'], stats['mid'], stats['deferral'],
                          stats['queued'], stats['constructor'], stats['combat']))


if __name__ == '__main__':
    main()
