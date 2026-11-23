#!/usr/bin/env python3
"""Compare the fire-at-will target picker with retail's 412620.

Per case it runs retail's picker 0x412620(shooter, slot 0, sweep) on a random
shooter and one to six enemy candidates: finished or with construction left
(+0x108 a fraction or 1.0), paralyzed or not (+0x114 bit 0x10), a bad target
or not (UnitDef +0x8e[slot]), immune to the stone, freeze and charm classes or
not, immune to paralyzers or not (UnitDef +0x260 bit 26), at random positions
in range, for a plain, melee, stone, freeze or charm weapon class, of the
paralyzer damage type (WeaponType +0xc8 & 0xf == 4) or not, with or without
fireatwillrandom, AI-owned or not, in the sweep
(radius: the weapon's range) and in the mission scan (4d8370's call: the
larger of sightdistance and the range).

Retail's own code runs throughout, the game RNG 0x535cc0 and the list erase
0x50b2c0 (it moves the last candidate into the drawn one's place) included.
Logged instead of run: the list query 0x411ae0 (it answers the case's
candidates in id order), the range 0x52aba0, the in-range test 0x52ad60 (every
candidate is in range), the minimum-range test 0x52adf0 (none is inside it),
the damage 0x531e50 (the weapon's damage) and the list's free 0x4eba00. The
shooter is a building (no mover: no turn penalty). The port (fire_sweep_test
--picker: World::retailSelectTarget on the same case) must pick the same
candidate and leave the same RNG state.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_fire_sweep_picker.py --binary build-o2/fire_sweep_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP

SHOOTER = HEAP + 0x1000
SDEF = HEAP + 0x2000         # the shooter's UnitDef
WEAPON = HEAP + 0x3000       # its slot-0 WeaponType
CLASSOBJ, VTABLE, STUBS = HEAP + 0x3400, HEAP + 0x3500, HEAP + 0x3600
PREC = HEAP + 0x3800         # its player record
GLOB_P, GLOB_Q = HEAP + 0x3a00, HEAP + 0x3b00
NOTARGET, BADTARGET = HEAP + 0x3c00, HEAP + 0x3d00
CANDS, CSTRIDE = HEAP + 0x10000, 0x140
CDEFS, DSTRIDE = HEAP + 0x20000, 0x300
LIST = HEAP + 0x30000
STUB_FALSE, STUB_TRUE = STUBS, STUBS + 0x10
CENTRE = 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/fire_sweep_test')
    ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--seed', type=int, default=0x412620)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
    put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
    putf = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    uc.mem_write(STUB_FALSE, bytes((0x31, 0xc0, 0xc3)))           # xor eax,eax; ret
    uc.mem_write(STUB_TRUE, bytes((0x31, 0xc0, 0xb0, 0x01, 0xc3)))  # xor eax,eax; mov al,1; ret
    put(0x62d558, GLOB_P); put(GLOB_P, GLOB_Q); put8(GLOB_Q + 7, 0)
    case = {}
    queries = []

    def query(uc_, sp):          # 411ae0(player, pos, radius, flag, out)
        player, pos, radius, flag, out = struct.unpack('<5I', uc.mem_read(sp, 20))
        queries.append((player, radius, flag))
        ptrs = [CANDS + i * CSTRIDE for i in range(len(case['cands']))]
        for i, a in enumerate(ptrs):
            put(LIST + 4 * i, a)
        put(out + 4, LIST); put(out + 8, LIST + 4 * len(ptrs)); put(out + 12, LIST + 4 * len(ptrs))
        return 5, 0

    p.hooks.update({
        0x411ae0: query,
        0x52aba0: lambda uc_, sp: (2, case['range']),
        0x52ad60: lambda uc_, sp: (3, 1),
        0x52adf0: lambda uc_, sp: (3, 0),
        0x531e50: lambda uc_, sp: (1, case['damage']),
        0x4eba00: lambda uc_, sp: (0, 0),
    })
    p.freeze_hooks()

    rng = random.Random(args.seed)
    rows, expected = [], []
    for n in range(args.cases):
        k = rng.randint(1, 6)
        cls = rng.choices((0, 1, 2, 3), (55, 15, 15, 15))[0]
        melee = int(cls == 0 and rng.random() < 0.25)
        c = {
            'seed': rng.getrandbits(32) | 1, 'sweep': rng.randint(0, 1), 'fawr': int(rng.random() < 0.3),
            'melee': melee, 'cls': cls, 'ai': rng.randint(0, 1), 'sight': rng.randint(0, 600),
            'range': rng.randint(160, 420), 'damage': rng.choice((1, 3, 10, 57, 250, 1000)),
            'para': int(rng.random() < 0.2),
        }
        # Candidates on distinct 48 px cells, in range, clear of the shooter.
        cells = set()
        cands = []
        while len(cands) < k:
            gx, gz = rng.randint(-8, 8), rng.randint(-8, 8)
            if (gx, gz) in cells or (abs(gx) < 2 and abs(gz) < 2):
                continue
            x, z = CENTRE + 48 * gx + rng.randint(-6, 6), CENTRE + 48 * gz + rng.randint(-6, 6)
            if (x - CENTRE) ** 2 + (z - CENTRE) ** 2 >= (c['range'] - 2) ** 2:
                continue
            cells.add((gx, gz))
            cands.append({'x': x, 'z': z, 'unfinished': rng.choice((0, 0, 0, 0.25, 1.0)),
                          'paralyzed': int(rng.random() < 0.2), 'bad': int(rng.random() < 0.2),
                          'immune': int(rng.random() < 0.2), 'pimmune': int(rng.random() < 0.25)})
        c['cands'] = cands
        case.clear(); case.update(c)

        # The shooter: a building of player 0 (no mover), its slot-0 weapon.
        uc.mem_write(SHOOTER, bytes(0x140)); uc.mem_write(SDEF, bytes(0x300)); uc.mem_write(WEAPON, bytes(0x100))
        uc.mem_write(PREC, bytes(0x100)); uc.mem_write(BADTARGET, bytes(0x40)); uc.mem_write(NOTARGET, bytes(0x40))
        put(SHOOTER + 0xb4, SDEF); put(SHOOTER + 0xb8, PREC); put8(SHOOTER + 0xfd, 0)
        put(SHOOTER + 0x68, CENTRE << 16); put(SHOOTER + 0x70, CENTRE << 16)
        put(SHOOTER + 0x130, 0x1000000 | 0x8000000 | 0x80000 | 1)
        put(SHOOTER + 0x0c, WEAPON)
        put(PREC, 1); put8(PREC + 0xea, 2 if c['ai'] else 1)
        put(SDEF + 0x1aa, WEAPON); put(SDEF + 0x264, 0x1000000 if c['fawr'] else 0)
        put16(SDEF + 0x226, c['sight']); put(SDEF + 0x9a, NOTARGET); put(SDEF + 0x8e, BADTARGET)
        put(WEAPON + 0x40, CLASSOBJ); put(CLASSOBJ, VTABLE)
        put(WEAPON + 0xc8, 4 if c['para'] else 0)    # damage type (low nibble): 4 paralyzer
        put(VTABLE + 0x14, STUB_TRUE if melee else STUB_FALSE)
        put(VTABLE + 0x1c, STUB_TRUE if cls == 1 else STUB_FALSE)
        put(VTABLE + 0x20, STUB_TRUE if cls == 2 else STUB_FALSE)
        put(VTABLE + 0x24, STUB_TRUE if cls == 3 else STUB_FALSE)
        for i, e in enumerate(cands):
            u, d = CANDS + i * CSTRIDE, CDEFS + i * DSTRIDE
            uc.mem_write(u, bytes(CSTRIDE)); uc.mem_write(d, bytes(DSTRIDE))
            put16(u, i + 1)                      # the type index the category masks test
            put(u + 0x130, 0x1000000 | 1)        # alive, on the ground
            put(u + 0xb4, d)
            putf(u + 0x108, e['unfinished'])
            put8(u + 0x114, 0x10 if e['paralyzed'] else 0)
            put(u + 0x68, e['x'] << 16); put(u + 0x70, e['z'] << 16)
            put(d + 0x260, 0x8000 | (0x4000000 if e['pimmune'] else 0))   # shootme, immunetoparalyzer
            put(d + 0x268, 6 if e['immune'] else 0)
            put(d + 0x264, 0x100000 if e['immune'] else 0)
            if e['bad']:
                word = BADTARGET + 4 * ((i + 1) >> 5)
                put(word, get(word) | (1 << ((i + 1) & 31)))
        put(0x64186c, c['seed'])
        queries.clear()
        result, error = p.call(0x412620, (SHOOTER, 0, c['sweep']))
        if error:
            raise RuntimeError(error)
        index = -1 if result == 0 else (result - CANDS) // CSTRIDE
        radius = c['range'] if c['sweep'] else max(c['sight'], c['range'])
        assert queries == [(0, radius, 0)], (n, queries, radius)
        expected.append(f'{index} {get(0x64186c)}')
        fields = [c['seed'], c['sweep'], c['fawr'], melee, cls, c['ai'], c['sight'], c['range'], c['damage'],
                  CENTRE, CENTRE, k, c['para']]
        for e in cands:
            fields += [e['x'], e['z'], int(e['unfinished'] > 0), e['paralyzed'], e['bad'], e['immune'], e['pimmune']]
        rows.append(' '.join(map(str, fields)))
    proc = subprocess.run([args.binary, '--picker'], input='\n'.join(rows) + '\n', text=True,
                          capture_output=True, check=True)
    actual = proc.stdout.splitlines()
    if len(actual) != len(expected):
        raise AssertionError(f'{len(actual)} answers for {len(expected)} cases')
    bad = [(i, rows[i], want, got) for i, (want, got) in enumerate(zip(expected, actual)) if want != got]
    if bad:
        for i, row, want, got in bad[:10]:
            print(f'case {i}: {row}\n  retail {want}  port {got}')
        raise SystemExit(f'FAIL: {len(bad)} of {len(rows)} picks differ')
    picked = sum(1 for e in expected if not e.startswith('-1 '))
    sites = sum(1 for i, e in enumerate(expected)
                if not e.startswith('-1 ') and case_unfinished(rows[i], int(e.split()[0])))
    print(f'PASS: {len(rows)} picks ({picked} with a target, {sites} of them a unit with construction left): '
          'the same candidate and the same RNG state as retail')


def case_unfinished(row, index):
    fields = row.split()
    return fields[13 + 7 * index + 2] == '1'


if __name__ == '__main__':
    main()
