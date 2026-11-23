#!/usr/bin/env python3
"""Compare the Classic brain's squad assignment with retail's.

Per case it runs, as the scheduler does when its countdown runs out, the
original dying-unit cleanup 0x40fe80 and then the assignment pass 0x40eeb0 on a
random player: up to 16 units with random flags (live, dying, selectable,
structure, armed, plus unrelated bits), commanders, sacred-site types and
builders, finished or not (+0x108 0, -0, 0.5 or a NaN), held (+0x104), carried
by a holder or not, with or without a navigator and a build menu, at random
positions and footprints, already in squads or not (97 and 98, the mass
attack's, among them); random base anchors (start and sacred-site records,
valid, empty or out of range), the first-pass flag, the tick (around the tick
<= 30 window and the unsigned wrap), the scenario mode (1 = a campaign mission,
with its own first-pass rules) and the massattack tick.

Everything under the two routines is retail's own code: set-squad 0x50b4f0
with the member vector's insert 0x409510 and swap-remove 0x50b300/0x50b2c0,
nearest base 0x40af70, the base centre 0x40c610 (anchor 0x40c750, centroid
0x40aa50, categories 0x40a5f0), the ring radius 0x40cf30, the first empty base
0x40af20 and the start anchor 0x40c670. Only the two order routines it calls
are logged instead of run: the standing orders 0x5198a0(u, 2) and the mission
clear 0x4d6a50(u, 0); operator new/delete are a bump allocator. The port
(classic_ai_test --assignment: ai::classicDropDying + ai::classicAssign) must
leave every unit's squad, every squad's member list in order, every base's
anchor and the first-pass flag as retail does, and make the same order calls
in the same order. The FPU runs at the C runtime's 53-bit precision (0x027f),
as in check_ai_profile.py.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_assignment.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

GAME = HEAP                  # game state: +0x19f44 tick, +0x175dc scenario, +0x3070 net flags
SCEN = HEAP + 0x20000        # +0 mode, +side*12+0xd55 massattack
PLAYER = HEAP + 0x22000
MGR = HEAP + 0x22400
SQOBJ = HEAP + 0x22800       # 100 squad objects, 0x20 apart
DATA = HEAP + 0x24000        # 100 squad data records, 0xc4 apart
VEC = HEAP + 0x29000         # member storage, 64 pointers per slot
UNITS = HEAP + 0x30000
TYPES = HEAP + 0x32000
HOLDER, NONHOLDER = HEAP + 0x38000, HEAP + 0x38200
NAV = HEAP + 0x39000
STARTS, SITES = HEAP + 0x3a000, HEAP + 0x3b000
ARENA = HEAP + 0x40000
USTRIDE, DSTRIDE, CAP = 0x138, 0xc4, 64
VTABLE = {'base': 0x5ebb2c, 'strike': 0x5ebb50, 'backup': 0x5ebb60, 'vtol': 0x5ebb70, 'reserve': 0x5ebb1c}
NOISE = [0x1, 0x2, 0x8, 0x40, 0x100, 0x400, 0x2000, 0x10000, 0x80000, 0x4000000, 0x10000000]


def kind(slot):
    if slot <= 20: return 'base'
    if slot <= 80: return 'strike' if slot % 2 else 'backup'
    if slot <= 95: return 'vtol'
    return {96: 'reserve', 97: 'strike', 98: 'backup'}[slot]


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

    def new(uc_, argv):
        size = struct.unpack('<I', uc_.mem_read(argv, 4))[0]
        at = heap[0]; heap[0] = (at + size + 15) & ~15
        return 0, at
    def unit_index(address):
        return (address - UNITS) // USTRIDE
    def standing(uc_, argv):
        u, arg = struct.unpack('<Ii', uc_.mem_read(argv, 8))
        log.append((unit_index(u), arg)); return 2, 0
    def clear(uc_, argv):
        u, arg = struct.unpack('<Ii', uc_.mem_read(argv, 8))
        log.append((unit_index(u), arg)); return 2, 0
    p.hooks[0x4eb9e0] = new
    p.hooks[0x4eba00] = lambda uc_, argv: (0, 0)
    p.hooks[0x5198a0] = standing
    p.hooks[0x4d6a50] = clear
    p.freeze_hooks()

    rng = random.Random(0x40eeb0)
    rows, expected = [], []
    for case in range(args.cases):
        heap[0] = ARENA; log.clear()
        uc.mem_write(GAME, bytes(0x1a000)); uc.mem_write(SCEN, bytes(0x1000))
        uc.mem_write(PLAYER, bytes(0x400)); uc.mem_write(MGR, bytes(0x1b0))
        uc.mem_write(UNITS, bytes(0x2000)); uc.mem_write(TYPES, bytes(0x6000))
        uc.mem_write(HOLDER, bytes(0x400))
        put(HOLDER + 0x130, 0x4000000 | 0x1000000)
        put(0x62d55c, GAME)
        tick = rng.choice([0, 1, 15, 29, 30, 31, 32, 100, 1000, 0x7fffffff, 0xffffffff, rng.getrandbits(32)])
        put(GAME + 0x19f44, tick)
        put(GAME + 0x175dc, SCEN)
        mode = rng.choice([0, 1, 1, 2, 2, 3])
        put(SCEN, mode)
        side = rng.randrange(10)
        mass = 0 if rng.random() < 0.5 else rng.choice(
            [tick, (tick + 1) & 0xffffffff, (tick - 1) & 0xffffffff, rng.randrange(1, 2000), 0xffffffff, 1])
        put(SCEN + side * 12 + 0xd55, mass)
        start = rng.choice([0, 1, 2, 3, 5, 9, 11, 12, 255, rng.randrange(256)])
        uc.mem_write(PLAYER + 0xec, bytes([start]))
        put(MGR, PLAYER); uc.mem_write(MGR + 4, bytes([side]))
        first = rng.random() < 0.3
        uc.mem_write(MGR + 0x1a6, bytes([int(first)]))

        def records(base, pointer):
            n = rng.choice([0, 0, 1, 2, 3, 4, 6, 10, 12])
            out = []
            for k in range(n):
                valid = rng.random() < 0.8
                x = rng.randrange(0, 160) if rng.random() < 0.9 else rng.randrange(-32768, 32768)
                z = rng.randrange(0, 160) if rng.random() < 0.9 else rng.randrange(-32768, 32768)
                uc.mem_write(base + k * 101, bytes([int(valid)]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((int(valid), x, z))
            empty_null = n == 0 and rng.random() < 0.5
            put(pointer, 0 if empty_null else base); put(pointer + 4, 0 if empty_null else base + n * 101)
            return out
        starts = records(STARTS, 0x62dbdc)
        sites = records(SITES, 0x62dbcc)

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
        anchors = []
        for slot in range(1, 21):
            a = 0 if rng.random() < 0.7 else rng.choice([rng.randrange(-8, 13), -2147483648, 2147483647])
            put(DATA + slot * DSTRIDE + 0xc, a); anchors.append(a)

        n = rng.choice([0, 1, 2, 3, 4, 6, 8, 10, 12, 16])
        put(PLAYER + 0x74, UNITS); put(PLAYER + 0x78, UNITS + (n - 1) * USTRIDE)
        members = {slot: [] for slot in range(1, 99)}
        units = []
        for i in range(n):
            u, t = UNITS + i * USTRIDE, TYPES + i * 0x300
            live = rng.random() < 0.92
            flags = (0x1000000 if live else 0)
            if rng.random() < 0.15: flags |= 0x1000
            if rng.random() < 0.7: flags |= 0x20
            if rng.random() < 0.3: flags |= 0x2000000
            if rng.random() < 0.5: flags |= 0x8000000
            for bit in NOISE:
                if rng.random() < 0.08: flags |= bit
            commander, sacred, builder = rng.random() < 0.25, rng.random() < 0.2, rng.random() < 0.25
            remaining = rng.choice([0.0, 0.0, 0.0, -0.0, 0.5, float('nan')])
            held = 0 if rng.random() < 0.85 else rng.choice([1, 0x80000000])
            carrier = rng.choice([0] * 8 + [1, 2])
            mover = rng.random() < 0.7
            menu = rng.choice([-1, 0, 0, 0, 1, 4])
            def coord():
                return rng.randrange(0, 2500 << 16) if rng.random() < 0.93 else rng.getrandbits(32) - (1 << 31)
            x, z = coord(), coord()
            fx, fz = rng.choice([1, 2, 3, 4, 6, 8, 12, -3, 32767]), rng.choice([1, 2, 3, 4, 6, 8, 12, -3, 32767])
            if flags & 0x1000000:
                r = rng.random()
                # 97/98 get a share of their own, so the massattack's skip of units
                # already there is exercised.
                squad = (0 if r < 0.45 else -1 if r < 0.55 else rng.randrange(1, 21) if r < 0.85 else
                         rng.choice([97, 98]) if r < 0.9 else rng.randrange(21, 99))
            else:
                squad = rng.choice([0, -1])
            if squad > 0: members[squad].append(i)
            put(u + 0x130, flags); put(u + 0xb4, t); put(u + 0xb8, PLAYER); put(u + 0xc8, squad)
            uc.mem_write(u + 0x108, struct.pack('<f', remaining)); put(u + 0x104, held)
            put(u + 0xa8, [0, HOLDER, NONHOLDER][carrier]); put(u + 8, NAV if mover else 0)
            put(u + 0x68, x); put(u + 0x6c, 0); put(u + 0x70, z)
            uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz))
            put(t + 0x264, (0x40000 if commander else 0) | (0x80000000 if sacred else 0))
            put(t + 0x260, 0x100 if builder else 0)
            uc.mem_write(t + 0x12e, struct.pack('<i', menu))
            f108 = struct.unpack('<I', struct.pack('<f', remaining))[0]
            units.append([flags, int(commander), int(sacred), int(builder), f108, int(held != 0), carrier,
                          int(mover), menu, x - (1 << 32) if x >= 1 << 31 else x, z - (1 << 32) if z >= 1 << 31 else z,
                          fx, fz, squad])
        for slot in range(1, 99):
            rng.shuffle(members[slot])
            d = DATA + slot * DSTRIDE
            begin = VEC + slot * CAP * 4
            if not members[slot] and rng.random() < 0.3:
                put(d + 0xb8, 0); put(d + 0xbc, 0); put(d + 0xc0, 0)
            else:
                for k, i in enumerate(members[slot]): put(begin + 4 * k, UNITS + i * USTRIDE)
                put(d + 0xb8, begin); put(d + 0xbc, begin + 4 * len(members[slot])); put(d + 0xc0, begin + 4 * CAP)

        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x40fe80, ecx=MGR)
        if error: raise RuntimeError(error)
        _, error = p.call(0x40eeb0, ecx=MGR)
        if error: raise RuntimeError(error)

        row = [uc.mem_read(MGR + 0x1a6, 1)[0]]
        row += [geti(DATA + slot * DSTRIDE + 0xc) for slot in range(1, 21)]
        row += [geti(UNITS + i * USTRIDE + 0xc8) for i in range(n)]
        for slot in range(1, 99):
            d = DATA + slot * DSTRIDE
            b, e = get(d + 0xb8), get(d + 0xbc)
            ids = [unit_index(get(a)) for a in range(b, e, 4)] if b else []
            row += [len(ids)] + ids
        row += [len(log)] + [v for entry in log for v in entry]
        expected.append(row)
        line = [tick, mode, mass, start, int(first), len(starts)] + [v for r in starts for v in r]
        line += [len(sites)] + [v for r in sites for v in r] + anchors + [n] + [v for u in units for v in u]
        for slot in range(1, 99):
            line += [len(members[slot])] + members[slot]
        rows.append(' '.join(map(str, line)))

    out = subprocess.run([args.binary, '--assignment'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        diff = [(k, a, b) for k, (a, b) in enumerate(zip(actual[i], expected[i])) if a != b][:8]
        raise AssertionError('%d of %d assignment passes differ; first: case %d, (index, port, retail) %s\n%s'
                             % (len(bad), len(rows), i, diff, rows[i]))
    firsts = sum(1 for r in rows if r.split()[4] == '0')
    print('PASS: %d cleanup + assignment passes (%d with the first pass pending; campaign mode, massattack, '
          'anchors, swap-remove order, order calls), 0 mismatches' % (len(rows), firsts))


if __name__ == '__main__':
    main()
