#!/usr/bin/env python3
"""Compare World::retailMissionScan with retail's mission scan 4d8370.

Per case it runs retail's 0x4d8370(shooter) on a random shooter and one to
seven enemy candidates: armed or not, on Hold Fire, Return Fire or Fire At
Will, one to three weapon slots (each ranged or melee, its own range), a
weapon switcher or not with a selected slot 0..2 or 3 (none), kamikaze or not, AI-owned
or not, fireatwillrandom or not, a paralyzer damage type or not; each
candidate on the owner's visible-enemy list +5 or not, in the air or on the
ground, with or without shootme, paralyzed or not, immune to paralyzers or
not, at a random distance inside, between or beyond the scanning slot's
range and the shooter's sightdistance.

Retail's own code runs throughout: the scan 0x4d8370, the slot choice
0x519ac0, the picker 0x412620, the list query 0x411ae0, the in-range test
0x52ad60 with the real weapon-class vtables (generic 0x5f3710, melee
0x5f3af8), the RNG 0x535cc0 and the list erase 0x50b2c0. Hooked: the damage
0x531e50 (the case's damage), 0x413290 (istargetingupgrade: no), operator
new 0x4eb9e0 (a bump heap) and the free 0x4eba00. The list +5 is written directly, in unit order (the rebuild
0x411700 and its sight test are tools/re/probe_mission_scan_vision.py's).
The shooter is a building (no mover, no turn penalty) and candidates keep at
least 4 px off the range and gather circles. The port (fire_sweep_test
--scan: World::retailMissionScan on the same case, the visible list set to
retail's) must pick the same candidate and leave the same RNG state.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_mission_scan.py --binary build-o2/fire_sweep_test
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

UNITS, USTRIDE = HEAP + 0x80000, 0x138
DEFS, DSTRIDE = HEAP + 0xa0000, 0x300
CACHE = HEAP + 0x100000
RECS = HEAP + 0x101000
WEAPONS = HEAP + 0x103000
CLASSOBJ = HEAP + 0x104000
GLOB_P, GLOB_Q = HEAP + 0x107000, HEAP + 0x107100
MASKS = HEAP + 0x107400
VEC = HEAP + 0x110000
SHOOTER_INDEX = 60
CENTRE = 1024
GENERIC, MELEE = 0x5f3710, 0x5f3af8
FIRE_BITS = (0, 0x40000, 0x80000)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/fire_sweep_test')
    ap.add_argument('--cases', type=int, default=4000)
    ap.add_argument('--seed', type=int, default=0x4d8370)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
    put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    put(0x62d558, GLOB_P); put(GLOB_P, GLOB_Q); put8(GLOB_Q + 7, 0)   # ShootAll off
    for i in range(2):
        r = RECS + 0x100 * i
        uc.mem_write(r, bytes(0x100)); put(r, 1); put8(r + 0xea, 1); put8(r + 0xeb, i)
    put8(RECS + 0xac, 1)               # player 0 is allied with itself only
    uc.mem_write(CACHE, bytes(0x100)); put(CACHE, RECS)
    for k, lst in enumerate((5, 0x15, 0x25, 0x35)):
        put(CACHE + lst + 4, VEC + 0x1000 * k); put(CACHE + lst + 8, VEC + 0x1000 * k)
        put(CACHE + lst + 0xc, VEC + 0x1000 * k + 0xff0)
    put(0x62a33c, CACHE)
    uc.mem_write(MASKS, bytes(0x40))
    case = {}
    bump = [HEAP + 0x200000]

    def new(uc_, sp):            # operator new: a bump heap (the query's vector growth)
        size = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        at = bump[0]
        bump[0] += (size + 15) & ~15
        if bump[0] >= HEAP + 0x3f0000:
            raise RuntimeError('bump heap exhausted')
        return 0, at             # cdecl: the caller pops the size
    p.hooks.update({
        0x4eb9e0: new,
        0x531e50: lambda uc_, sp: (1, case['damage']),
        0x413290: lambda uc_, sp: (1, 0),
        0x4eba00: lambda uc_, sp: (0, 0),
    })
    p.freeze_hooks()

    def clear_unit(i):
        u, d = UNITS + i * USTRIDE, DEFS + i * DSTRIDE
        uc.mem_write(u, bytes(USTRIDE)); uc.mem_write(d, bytes(DSTRIDE)); put(u + 0xb4, d)
        return u, d

    rng = random.Random(args.seed)
    rows, expected, stats = [], [], {'picked': 0, 'listedOutside': 0}
    for n in range(args.cases):
        nslots = rng.choice((1, 1, 2, 3))
        slots = [{'range': rng.randint(40, 700), 'melee': int(rng.random() < 0.25)} for _ in range(nslots)]
        c = {
            'seed': rng.getrandbits(32) | 1,
            'fire': rng.choices((0, 1, 2), (10, 10, 80))[0],
            'armed': int(rng.random() < 0.93),
            'switcher': int(rng.random() < 0.3),
            'fawr': int(rng.random() < 0.3), 'ai': rng.randint(0, 1),
            'kamikaze': int(rng.random() < 0.08), 'sight': rng.randint(0, 700),
            'damage': rng.choice((1, 3, 10, 57, 250, 1000)), 'para': int(rng.random() < 0.15),
            'slots': slots,
        }
        # A switcher's selected slot: one it has, or 3 (519ac0 then takes the
        # first slot; its fallback returns the slot in al alone, the picker
        # reads the low byte).
        c['sel'] = rng.choice(list(range(nslots)) + [3]) if c['switcher'] else 0
        # The scanning slot (519ac0), for the case geometry only; the port
        # and retail each work it out themselves.
        srange = slots[c['sel'] if c['sel'] < 3 else 0]['range']
        radius = max(c['sight'], srange)
        k = rng.randint(1, 7)
        cells, cands = set(), []
        while len(cands) < k:
            dist = rng.uniform(40, 1.3 * max(radius, 60))
            if any(abs(dist - b) < 4 for b in (srange, radius)):
                continue
            angle = rng.uniform(0, 2 * math.pi)
            x, z = CENTRE + round(dist * math.cos(angle)), CENTRE + round(dist * math.sin(angle))
            d = math.hypot(x - CENTRE, z - CENTRE)
            if any(abs(d - b) < 4 for b in (srange, radius)) or (x >> 4, z >> 4) in cells:
                continue
            cells.add((x >> 4, z >> 4))
            cands.append({'x': x, 'z': z, 'listed': int(rng.random() < 0.8), 'flyer': int(rng.random() < 0.15),
                          'shootme': int(rng.random() < 0.85), 'paralyzed': int(rng.random() < 0.15),
                          'pimmune': int(rng.random() < 0.2)})
        c['cands'] = cands
        case.clear(); case.update(c)

        # The shooter: a building of player 0, its slots at +0xc/+0x28/+0x44
        # (and UnitDef +0x1aa.. for the picker's slot weapon lookups).
        sh, sd = clear_unit(SHOOTER_INDEX)
        bits = 0x1000000 | FIRE_BITS[c['fire']] | 1 | (c['sel'] << 30)
        if c['armed']:
            bits |= 0x8000000
        put(sh + 0x130, bits)
        put(sh + 0xb8, RECS); put8(sh + 0xfd, 0)
        put(sh + 0x68, CENTRE << 16); put(sh + 0x70, CENTRE << 16)
        put8(RECS + 0xea, 2 if c['ai'] else 1)
        put16(sd + 0x226, c['sight'])
        flags260 = (0x10000 if c['switcher'] else 0) | (0x10000000 if c['kamikaze'] else 0)
        put(sd + 0x260, flags260); put(sd + 0x264, 0x1000000 if c['fawr'] else 0)
        for off in (0x9a, 0x8e, 0x92, 0x96):
            put(sd + off, MASKS)
        for s, sl in enumerate(slots):
            w = WEAPONS + 0x100 * s
            uc.mem_write(w, bytes(0x100))
            obj = CLASSOBJ + 0x10 * s
            put(obj, MELEE if sl['melee'] else GENERIC)
            put(w + 0x40, obj); put(w + 0x90, sl['range']); put(w + 0x94, 0)
            put(w + 0xc8, 4 if c['para'] else 0)
            put(sh + 0xc + 0x1c * s, w); put(sd + 0x1aa + 4 * s, w)
        # The candidates: enemies of player 1, units 1..k.
        listed = []
        for i, e in enumerate(cands):
            u, d = clear_unit(i + 1)
            put16(u, i + 1)
            put(u + 0x130, 0x1000000 | (2 if e['flyer'] else 1))
            put(u + 0xb8, RECS + 0x100); put8(u + 0xfd, 1)
            put(u + 0x68, e['x'] << 16); put(u + 0x70, e['z'] << 16)
            put8(u + 0x114, 0x10 if e['paralyzed'] else 0)
            put(d + 0x260, (0x8000 if e['shootme'] else 0) | (0x4000000 if e['pimmune'] else 0))
            if e['listed']:
                listed.append(u)
                if math.hypot(e['x'] - CENTRE, e['z'] - CENTRE) > srange:
                    stats['listedOutside'] += 1
        for j, u in enumerate(listed):
            put(VEC + 4 * j, u)
        put(CACHE + 9, VEC); put(CACHE + 0xd, VEC + 4 * len(listed))
        put(0x64186c, c['seed'])
        bump[0] = HEAP + 0x200000
        result, error = p.call(0x4d8370, (sh,))
        if error:
            raise RuntimeError(f'case {n}: {error}: {c}')
        index = -1 if result == 0 else (result - UNITS) // USTRIDE - 1
        expected.append(f'{index} {get(0x64186c)}')
        stats['picked'] += index >= 0
        fields = [c['seed'], c['fire'], c['armed'], c['switcher'], c['sel'], c['fawr'], c['ai'], c['kamikaze'],
                  c['sight'], c['damage'], c['para'], CENTRE, CENTRE, nslots]
        for sl in slots:
            fields += [sl['range'], sl['melee']]
        fields.append(k)
        for e in cands:
            fields += [e['x'], e['z'], e['listed'], e['flyer'], e['shootme'], e['paralyzed'], e['pimmune']]
        rows.append(' '.join(map(str, fields)))
    proc = subprocess.run([args.binary, '--scan'], input='\n'.join(rows) + '\n', text=True,
                          capture_output=True, check=True)
    actual = proc.stdout.splitlines()
    if len(actual) != len(expected):
        raise AssertionError(f'{len(actual)} answers for {len(expected)} cases')
    bad = [(i, rows[i], want, got) for i, (want, got) in enumerate(zip(expected, actual)) if want != got]
    if bad:
        for i, row, want, got in bad[:10]:
            print(f'case {i}: {row}\n  retail {want}  port {got}')
        raise SystemExit(f'FAIL: {len(bad)} of {len(rows)} scans differ')
    print(f'PASS: {len(rows)} scans ({stats["picked"]} with a target; {stats["listedOutside"]} listed candidates '
          'beyond the scanning slot\'s range): the same candidate and the same RNG state as retail')


if __name__ == '__main__':
    main()
