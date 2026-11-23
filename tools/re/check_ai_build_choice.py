#!/usr/bin/env python3
"""Compare original weighted build selection, faction rejection and RNG order.

The availability/weight query is controlled; selection executes unchanged.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    game, unit, builder, types, menu, flags, faction = [HEAP + n * 0x30000 for n in range(7)]
    def word(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]
    word(0x62d55c, game)
    word(game + 0x175c4, types)
    word(unit + 0xb4, builder)
    word(builder + 0x132, menu)
    word(builder + 0x8a, faction)
    p.uc.mem_write(faction, b'Veruna\0Aramon\0')
    choices = []
    p.hooks[0x412c90] = lambda uc, argv: (2, choices[(get(argv + 4) & 65535)-1][1])
    p.freeze_hooks()
    calls = []
    def observe(uc, address, size, data):
        calls.append(struct.unpack('<i', uc.mem_read(uc.reg_read(UC_X86_REG_ESP)+4, 4))[0])
    p.uc.hook_add(UC_HOOK_CODE, observe, begin=0x535cc0, end=0x535cc0)
    rng = random.Random(0x412d00)
    rows, expected = [], []
    for case in range(4096):
        count, special, seed = rng.randrange(33), rng.randrange(2), rng.getrandbits(32)
        choices = [(i+1, rng.choice([-100, 0, 1, 2, 10, 100, 10000]), rng.randrange(2), rng.randrange(2))
                   for i in range(count)]
        word(builder + 0x12e, count)
        for i, (ident, weight, kind, same) in enumerate(choices):
            p.uc.mem_write(menu + i*2, struct.pack('<H', ident))
            word(types + ident*676 + 0x12a, flags + ident)
            p.uc.mem_write(flags + ident, bytes([128 if kind else 0]))
            word(types + ident*676 + 0x8a, faction if same else faction+7)
        word(0x64186c, seed)
        calls.clear()
        result, error = p.call(0x412d00, (0, unit, special))
        if error:
            raise RuntimeError(error)
        expected.append([result & 65535, get(0x64186c), len(calls), *calls])
        rows.append(' '.join(map(str, [count, special, seed, *(v for choice in choices for v in choice)])))
    result = subprocess.run([args.binary, '--build-choice'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [list(map(int, row.split())) for row in result.stdout.splitlines()]
    if actual != expected:
        raise AssertionError(next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 4096 original build selections, weighted draw order, special filters and faction rejection')


if __name__ == '__main__':
    main()
