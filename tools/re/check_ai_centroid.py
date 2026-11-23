#!/usr/bin/env python3
"""Compare unanchored squad centres with the original executable."""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    p.freeze_hooks()
    squad, data, members, output = HEAP, HEAP + 0x100, HEAP + 0x400, HEAP + 0x800
    def word(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    word(squad + 8, data)
    rng = random.Random(0x40aa50)
    rows, expected = [], []
    for case in range(4096):
        count = rng.randrange(33)
        row = [count]
        word(data + 0xb8, members)
        word(data + 0xbc, members + count * 4)
        for i in range(count):
            unit = HEAP + 0x1000 + i * 0x200
            word(members + i * 4, unit)
            flags = rng.choice([0, 0x1000000, 0x3000000, 0x1001000])
            x, z = [rng.randrange(-2**31, 2**31) for _ in range(2)]
            word(unit + 0x130, flags)
            word(unit + 0x68, x)
            word(unit + 0x70, z)
            row.extend([int(bool(flags & 0x1000000) and not(flags & 0x1000)), x, z])
        result, error = p.call(0x40aa50, (output, 0), ecx=squad)
        if error:
            raise RuntimeError(error)
        valid = result & 255
        x, _, z = struct.unpack('<iii', p.uc.mem_read(output, 12))
        expected.append((valid, x if valid else 0, z if valid else 0))
        rows.append(' '.join(map(str, row)))
    result = subprocess.run([args.binary, '--centroid'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, row.split())) for row in result.stdout.splitlines()]
    if actual != expected:
        raise AssertionError(next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 4096 original squad centres, mixed structures/mobile units, invalid members and signed rounding')


if __name__ == '__main__':
    main()
