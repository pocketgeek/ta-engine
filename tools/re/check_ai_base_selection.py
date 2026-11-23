#!/usr/bin/env python3
"""Compare nearest AI base selection with original distance arithmetic."""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    manager, position, table = HEAP, HEAP + 0x800, HEAP + 0x900
    objects = HEAP + 0x1000
    candidates = []
    def word(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    def index(uc):
        return (uc.reg_read(UC_X86_REG_ECX) - objects) // 0x400
    def center(uc, argv):
        occupied, valid, x, z, count = candidates[index(uc)]
        out = struct.unpack('<I', uc.mem_read(argv, 4))[0]
        uc.mem_write(out, struct.pack('<iii', x, 0, z))
        return 1, valid
    p.hooks[0x40a9b0] = lambda uc, argv: (1, candidates[index(uc)][4])
    p.hooks[HEAP + 0xa00] = center
    word(table + 8, HEAP + 0xa00)
    p.freeze_hooks()
    rng = random.Random(0x40af70)
    rows, expected = [], []
    for case in range(4096):
        x, z = [rng.randrange(-2**31, 2**31) for _ in range(2)]
        minimum = rng.randrange(-1, 8)
        candidates = []
        for i in range(20):
            bx, bz = [rng.randrange(-2**31, 2**31) for _ in range(2)]
            if case % 4 == 0: bx, bz = x, z  # ties retain the first eligible base
            candidates.append((rng.randrange(2), rng.randrange(2), bx, bz, rng.randrange(10)))
            obj = objects + i * 0x400
            word(manager + 0x15 + i * 4, obj)
            word(obj, table); word(obj + 8, obj + 0x100)
            word(obj + 0x100 + 0xb8, obj + 0x300)
            word(obj + 0x100 + 0xbc, obj + 0x300 + 4 * candidates[-1][0])
        p.uc.mem_write(position, struct.pack('<iii', x, 0, z))
        result, error = p.call(0x40af70, (manager, position, minimum))
        if error: raise RuntimeError(error)
        expected.append(result)
        rows.append(' '.join(map(str, [x, z, minimum, *(v for base in candidates for v in base)])))
    output = subprocess.run([args.binary, '--nearest-base'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True).stdout
    actual = list(map(int, output.split()))
    if actual != expected:
        raise AssertionError(next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 4096 original nearest-base selections, empty/filter cases, signed distances and ties')


if __name__ == '__main__':
    main()
