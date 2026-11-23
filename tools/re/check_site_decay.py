#!/usr/bin/env python3
"""Compare an abandoned site's decay with retail, tick by tick.

Retail runs the site's own GetBuilt (402220) under the primary dispatcher
(4d8450) every tick; nothing is hooked on that path but the weapon stop
(519b70) and the death call (51a140). Work events (429af0 sets 0x10000000 for
any work >= 0) are raised before or after the site's dispatch, as a builder in
an earlier or later unit slot would. The decay itself is the real 429d70 ->
429af0(site, site, -0.5): the refund lands in the SITE owner's resources
(owner+0x10c), and the death call is checked as 51a140(site, site, 30000, 11, 0).
The port side is `retail_construction_test --site-timeline` (retailSiteTimer
and retailSiteDecay, which World::tick's decay pass runs for every game).
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

GS, UNIT, OWNER, OTHER, TYPE, DEFS, MISSION, RES, OTHER_RES = (HEAP + n for n in
    (0, 0x20000, 0x21000, 0x21800, 0x22000, 0x23000, 0x24000, 0x26000, 0x27000))
WORK = 0x10000000


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def retail(ticks, inverse, cost, maxhp, remaining, stored, start, schedule):
    icd = Icd(); uc = icd.uc; uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    real = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
    byte = lambda a, v: uc.mem_write(a, bytes((v & 255,)))
    put(0x62d55c, GS)
    put(UNIT + 0x130, 0x1000000); put(UNIT + 0xd0, 0)
    put(UNIT + 0xb8, OWNER); put(UNIT + 0xb4, TYPE)
    real(UNIT + 0x108, remaining); put(UNIT + 0x10c, 0)
    put(OWNER, 1); byte(OWNER + 0xea, 1); put(OWNER + 0x10c, RES)
    put(OTHER, 1); byte(OTHER + 0xea, 1); put(OTHER + 0x10c, OTHER_RES)
    real(RES, stored); real(RES + 8, 1.0); real(OTHER_RES, 0.0)
    byte(TYPE + 0x24b, 0)
    real(TYPE + 0x216, inverse); real(TYPE + 0x20e, cost); put(TYPE + 0x1be, maxhp)
    put(0x62db84, DEFS); put(DEFS + 25 + 4, 0x402220)
    a = MISSION; byte(a + 4, 1); byte(a + 5, 0)
    for offset, value in ((6, 0), (10, 0xffffffff), (14, UNIT), (0x6a, 0), (0x5a, 0), (0x66, 0)):
        put(a + offset, value)
    put(UNIT + 0x60, MISSION)
    deaths = []

    def stop(uc_, sp):
        return 1, 0

    def death(uc_, sp):
        deaths.append(tuple(get(sp + 4 * i) for i in range(5)))
        put(UNIT + 0x130, 0)   # the death clears the live bit; the queue goes with it
        return 5, 0

    def fail(name):
        def hook(uc_, sp):
            raise AssertionError(f'unexpected {name}')
        return hook
    icd.hooks[0x519b70] = stop; icd.hooks[0x51a140] = death
    icd.hooks[0x429990] = fail('completion')
    icd.hooks[0x4d6ad0] = fail('remove'); icd.hooks[0x4d6b30] = fail('rotate')
    def clear(uc_, sp):
        if not deaths: raise AssertionError('queue cleared while alive')
        put(UNIT + 0x60, 0); return 2, 0
    icd.hooks[0x4d6a50] = clear
    icd.freeze_hooks()
    out = []
    for t in range(ticks):
        early, late = schedule[t]
        put(GS + 0x19f44, start + t)
        if not deaths:
            if early: put(UNIT + 0xd0, get(UNIT + 0xd0) | WORK)
            _, error = icd.call(0x4d8450, (UNIT,))
            if error: raise RuntimeError(error)
            if late: put(UNIT + 0xd0, get(UNIT + 0xd0) | WORK)
        if real_get(uc, OTHER_RES) != 0.0: raise AssertionError('refund reached another player')
        out += [get(UNIT + 0x108), get(RES), int(bool(deaths))]
    for d in deaths:
        if d != (UNIT, UNIT, 30000, 11, 0): raise AssertionError(('death call', d))
    return out


def real_get(uc, a):
    return struct.unpack('<f', uc.mem_read(a, 4))[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_construction_test')
    ap.add_argument('--cases', type=int, default=200)
    args = ap.parse_args()
    rng = random.Random(0x429d70)
    rows, expected = [], []
    for case in range(args.cases):
        kind = case % 5
        ticks = rng.choice([40, 340, 400, 700])
        inverse = f32(1.0 / rng.choice([100, 239, 1622, 3000, 0.5, rng.uniform(1, 5000)]))
        cost = f32(rng.choice([0, 55, 1367, 2116, rng.uniform(0, 9000)]))
        maxhp = rng.randrange(1, 30001)
        remaining = f32(rng.choice([1.0, 0.999, 0.5, 0.0001, rng.random()]))
        stored = f32(rng.choice([0.0, 12.5, rng.random() * 5000]))
        start = rng.choice([0, 1, 1000, rng.getrandbits(31)])  # no 32-bit tick wrap (years of play)
        if kind == 0:     # never worked: 300 ticks, then 30, then decay
            schedule = [(0, 0)] * ticks
        elif kind == 1:   # worked by an earlier slot until a random tick
            stop_at = rng.randrange(ticks)
            schedule = [(int(t < stop_at), 0) for t in range(ticks)]
        elif kind == 2:   # worked by a later slot until a random tick
            stop_at = rng.randrange(ticks)
            schedule = [(0, int(t < stop_at)) for t in range(ticks)]
        elif kind == 3:   # intermittent work, both slots
            p = rng.choice([0.01, 0.03, 0.2])
            schedule = [(int(rng.random() < p), int(rng.random() < p)) for _ in range(ticks)]
        else:             # a gap shorter than 30 ticks, then abandonment
            gap = rng.randrange(1, 40); a = rng.randrange(5, 60)
            schedule = [(int(t < a or a + gap <= t < a + gap + 3), 0) for t in range(ticks)]
        expected.append(retail(ticks, inverse, cost, maxhp, remaining, stored, start, schedule))
        rows.append(f'{ticks} {bits(inverse)} {bits(cost)} {bits(remaining)} {bits(stored)} {start}\n' +
                    ''.join(f'{e} {l}\n' for e, l in schedule))
    proc = subprocess.run([args.binary, '--site-timeline'], input=''.join(rows), text=True,
                          capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in proc.stdout.splitlines()]
    if len(actual) != len(expected): raise AssertionError('row count')
    for i, (want, got) in enumerate(zip(expected, actual)):
        if want != got:
            k = next(j for j in range(len(want)) if want[j] != got[j])
            raise AssertionError({'case': i, 'tick': k // 3, 'header': rows[i].split('\n')[0],
                                  'retail': want[k - k % 3:k - k % 3 + 3], 'port': got[k - k % 3:k - k % 3 + 3]})
    decays = sum(1 for want in expected for j in range(0, len(want) - 3, 3) if want[j] != want[j + 3])
    removals = sum(want[-1] for want in expected)
    print(f'PASS: {len(expected)} site timelines, {decays} decay ticks, {removals} removals; '
          'timers, refunds (to the site owner) and the kind-11 death')


if __name__ == '__main__':
    main()
