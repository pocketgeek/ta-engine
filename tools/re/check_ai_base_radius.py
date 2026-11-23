#!/usr/bin/env python3
"""Check integer AI base-radius arithmetic against original x87 calculations."""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    squad, table, output = HEAP, HEAP + 0x100, HEAP + 0x200
    p.uc.mem_write(squad, struct.pack('<I', table))
    p.uc.mem_write(table + 8, struct.pack('<I', HEAP + 0x300))
    area = 0
    p.hooks[HEAP + 0x300] = lambda uc, argv: (1, 1)
    p.hooks[0x40a940] = lambda uc, argv: (1, area)
    p.freeze_hooks()
    rng = random.Random(0x40cf30)
    cases = [(n, c) for n in range(512) for c in range(6)]
    cases += [(rng.randrange(2**31), rng.randrange(8)) for _ in range(4096)]
    expected = []
    for area, category in cases:
        result, error = p.call(0x40cf30, (category, output), ecx=squad)
        if error or not(result & 255):
            raise RuntimeError(error or 'radius query failed')
        expected.append(struct.unpack('<i', p.uc.mem_read(output, 4))[0])
    result = subprocess.run([args.binary, '--base-radius'],
                            input=''.join(f'{n} {c}\n' for n, c in cases),
                            text=True, capture_output=True, check=True)
    actual = list(map(int, result.stdout.split()))
    if actual != expected:
        raise AssertionError(next(((cases[i], a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print(f'PASS: {len(cases)} original AI base radii, footprint thresholds and large integer areas')


if __name__ == '__main__':
    main()
