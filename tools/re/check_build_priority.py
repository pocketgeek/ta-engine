#!/usr/bin/env python3
"""Compare 410810's build scores and complete RNG call sequence.

Availability classification is a controlled callback. Resource calculations,
score branches and random draws execute in the original routine.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, manager, owner, resource, types, arrays = [HEAP+n*0x30000 for n in range(6)]
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    real = lambda a, v: p.uc.mem_write(a, struct.pack('<f', v))
    short = lambda a, v: p.uc.mem_write(a, struct.pack('<h', v))
    put(0x62d55c, game)
    put(game+0x175b8, 2)
    put(game+0x175c4, types)
    put(manager, owner)
    put(owner+0x10c, resource)
    for offset, address in ((0x65, arrays), (0xa1, arrays+0x100)):
        put(manager+offset+4, address)
        put(manager+offset+8, address+2)
        put(manager+offset+12, address+2)
    for offset, address in ((0x85, arrays+0x200), (0x95, arrays+0x300), (0xe5, arrays+0x400)):
        put(manager+offset, address)
    p.hooks[0x4107e0] = lambda uc, sp: (1, 0)
    p.freeze_hooks()
    calls = []
    def observe(uc, address, size, user):
        sp = uc.reg_read(UC_X86_REG_ESP)
        calls.append(struct.unpack('<i', uc.mem_read(sp+4, 4))[0])
    p.uc.hook_add(UC_HOOK_CODE, observe, begin=0x535cc0, end=0x535cc0)
    rng = random.Random(0x410810)
    rows, expected = [], []
    for i in range(8192):
        flags = rng.choice([0, 0x100, 0x10000, 0x10100, 0x1000100])
        secondary = rng.choice([0, 0x80000000])
        count = rng.choice([-1, 0, 1, 2, 5, 17, 32767])
        completed = count-rng.randrange(2)
        weapon = rng.choice([-1, 0, 4])
        desired = rng.choice([-1, 0, 1, 2, 3, 4, 8, 24, 2147483647])
        cost = rng.choice([0., 499., 500., 500.03125, 599.999, 600., 601., 2000., rng.randrange(100000)])
        cost = struct.unpack('<f', struct.pack('<f', cost))[0]
        population = rng.choice([0, 9, 10, 29, 39, 40, 79, 80, 159, 160, 65535])
        ratio = rng.choice([0., 0.009, 0.01, 0.0101, 0.299, 0.3, 0.301, 1.])
        ratio = struct.unpack('<f', struct.pack('<f', ratio))[0]
        shortfall = i % 2
        seed = rng.getrandbits(32)
        put(types+676+0x260, flags)
        put(types+676+0x264, secondary)
        short(types+676+0x194, weapon)
        real(types+676+0x20e, cost)
        short(arrays+0x202, count)
        short(arrays+0x302, completed)
        put(arrays+0x404, desired)
        p.uc.mem_write(owner+0xe8, struct.pack('<H', population))
        real(resource, ratio)
        real(resource+4, 1.)
        real(resource+0x2c, 0. if shortfall else 100.)
        real(resource+0x30, 100. if shortfall else 0.)
        put(0x64186c, seed)
        calls.clear()
        _, error = p.call(0x410810, ecx=manager)
        if error:
            raise RuntimeError(error)
        expected.append([p.uc.mem_read(arrays+1, 1)[0], get(0x64186c), len(calls), *calls])
        rows.append(' '.join(map(str, [flags, secondary, count, completed, weapon, desired,
                                     cost, population, ratio, shortfall, seed])))
    proc = subprocess.run([args.binary, '--priority'], input='\n'.join(rows)+'\n',
                          text=True, capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in proc.stdout.splitlines()]
    if actual != expected:
        for row, want, got in zip(rows, expected, actual):
            if want != got:
                raise AssertionError((row, want, got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} build priorities, RNG bounds/order and final seeds')


if __name__ == '__main__':
    main()
