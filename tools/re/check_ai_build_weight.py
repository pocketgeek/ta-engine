#!/usr/bin/env python3
"""Compare native build availability limits and signed priority weights."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd(); p.freeze_hooks()
    game, manager, arrays = HEAP, HEAP+0x20000, HEAP+0x30000
    def word(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    word(0x62d55c, game); word(0x62a33c, manager)
    word(game+0x175b8, 2)
    for offset, address in [(0x85, arrays), (0xe5, arrays+0x100),
                            (0xc5, arrays+0x200), (0x69, arrays+0x300)]:
        word(manager+offset, address)
    rng = random.Random(0x412c90)
    rows, expected = [], []
    for case in range(8192):
        limited = case % 2
        desired = rng.choice([-1, 0, 1, 32767, rng.randrange(-2**31, 2**31)])
        count = rng.randrange(-32768, 32768)
        preference, priority = rng.randrange(256), rng.randrange(-128, 128)
        p.uc.mem_write(game+0x24e7, bytes([limited]))
        p.uc.mem_write(arrays+2, struct.pack('<h', count))
        word(arrays+0x104, desired)
        p.uc.mem_write(arrays+0x201, bytes([preference]))
        p.uc.mem_write(arrays+0x301, struct.pack('<b', priority))
        result, error = p.call(0x412c90, (0, 1))
        if error: raise RuntimeError(error)
        expected.append(struct.unpack('<i', struct.pack('<I', result))[0])
        rows.append(f'{limited} {desired} {count} {preference} {priority}')
    result = subprocess.run([args.binary, '--build-weight'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = list(map(int, result.stdout.split()))
    if actual != expected:
        raise AssertionError(next(((rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 8192 original build weights, availability limits and signed priority arithmetic')


if __name__ == '__main__':
    main()
