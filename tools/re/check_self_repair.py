#!/usr/bin/env python3
"""Compare healtime self-repair with 51e1a8 -> 429d90 -> 51a140 -> 51a240 (type 12).

Every eighth tick retail's per-unit update (51e152..51e1bc) passes a finished,
living unit below full health whose type has a nonzero healtime to the shared
repair routine 429d90, unpaid, with healtime * 0.26666664 (eight thirtieths of a
second of its own worker time). 429d90 turns that work into hit points through a
16.16 product whose bits 8..15 carry over in the unit's byte at +0x10e, and hands
the whole points to 51a140 as damage type 12, which 51a240 adds and clamps.

The emulated path runs every one of those routines; nothing is substituted.
Each case is a run of consecutive calls on one unit, so the carry is checked as
it accumulates.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_FPCW

GAME = 0x62d55c          # game-state pointer
CALL_SITE, AFTER = 0x51e1a8, 0x51e1c1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/retail_construction_test')
    args = ap.parse_args()
    p = Icd()
    p.freeze_hooks()
    uc = p.uc
    uc.reg_write(UC_X86_REG_FPCW, 0x027f)      # MSVC default: 53-bit, round to nearest
    game, kind, owner = HEAP, HEAP + 0x30000, HEAP + 0x31000
    units = HEAP + 0x32000                     # slot 0 unused; the unit is slot 1
    unit = units + 0x138
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get16 = lambda a: struct.unpack('<H', uc.mem_read(a, 2))[0]
    get8 = lambda a: uc.mem_read(a, 1)[0]
    real = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
    put(GAME, game)
    put(game + 0x14e84, units)
    put(game + 0x14e88, unit + 0x138)
    uc.mem_write(unit + 2, struct.pack('<H', 1))  # unit id -> slot 1
    put(unit + 0xb4, kind)
    put(unit + 0xb8, owner)                      # owner word 0 = no network echo
    put(owner, 0)
    put(unit + 0x130, 0x1000000)                 # allocated, not dying
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]

    def call(hp, carry, heal, inverse, maxhp):
        uc.mem_write(unit + 0x10c, struct.pack('<H', hp & 0xffff))
        uc.mem_write(unit + 0x10e, bytes([carry]))
        real(kind + 0x21e, heal)
        real(kind + 0x216, inverse)
        put(kind + 0x1be, maxhp)
        uc.reg_write(UC_X86_REG_ESP, STACK + STACK_SZ - 0x1000)
        uc.reg_write(UC_X86_REG_ECX, kind)
        uc.reg_write(UC_X86_REG_ESI, unit)
        uc.emu_start(CALL_SITE, AFTER, timeout=5_000_000)
        if uc.reg_read(UC_X86_REG_EIP) != AFTER:
            raise RuntimeError('self-repair call did not return')
        return get16(unit + 0x10c), get8(unit + 0x10e)

    rng = random.Random(0x51e1a8)
    rows, expected = [], []
    # Shipped shapes first: (maxdamage, buildtime, healtime) of the Crusades
    # Aramon Keep, Barracks, Arrow Tower, Lodestone, Divine Lodestone, an archer,
    # the monarch, and a zero-healtime wall.
    shipped = [(25116, 1622, 3.17083), (17162, 600, 1.25), (5200, 300, 0.625),
               (5680, 92, 0.283333333), (17092, 550, 2.175), (1100, 125, 0.520833333),
               (16000, 100000, 216.67), (3000, 40, 0.0)]
    cases = []
    for maxhp, bt, heal in shipped:
        cases.append((maxhp, f32(1.0 / bt), f32(heal), rng.randrange(1, maxhp), 0, 240))
    for _ in range(4000):
        maxhp = rng.randrange(1, 30001)
        bt = rng.choice([1, 10, 40, 92, 125, 600, 1622, 100000, rng.uniform(0.5, 120000)])
        heal = rng.choice([0.0, 0.001, 0.0416666679, 0.283333333, 1.25, 3.17083, 216.67,
                           600.0, rng.uniform(0, 50)])
        hp = rng.choice([0, 1, maxhp - 1, maxhp, maxhp + 1, 0x8000, 0xffff, rng.randrange(maxhp)])
        cases.append((maxhp, f32(1.0 / bt), f32(heal), hp, rng.randrange(256), rng.choice([1, 1, 3, 30])))
    for maxhp, inverse, heal, hp0, carry0, steps in cases:
        hp, carry, trail = hp0 & 0xffff, carry0, []
        for _ in range(steps):
            hp, carry = call(hp, carry, heal, inverse, maxhp)
            trail += [hp, carry]
        expected.append(trail)
        hb = struct.unpack('<I', struct.pack('<f', heal))[0]
        ib = struct.unpack('<I', struct.pack('<f', inverse))[0]
        rows.append(f'{hp0 & 0xffff} {carry0} {hb} {ib} {maxhp} {steps}')
    proc = subprocess.run([args.binary, '--self-repair'], input='\n'.join(rows) + '\n',
                          text=True, capture_output=True, check=True)
    actual = [list(map(int, row.split())) for row in proc.stdout.splitlines()]
    if actual != expected:
        for row, want, got in zip(rows, expected, actual):
            if want != got:
                raise AssertionError((row, want[:8], got[:8]))
        raise AssertionError('row count')
    calls = sum(c[5] for c in cases)
    print(f'PASS: {len(cases)} units, {calls} self-repair calls: hit points and carry byte bit-exact')


if __name__ == '__main__':
    main()
