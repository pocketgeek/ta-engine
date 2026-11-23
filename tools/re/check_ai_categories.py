#!/usr/bin/env python3
"""Check squad planner membership categories against executable behaviour."""
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
    unit, kind, mission = HEAP, HEAP + 0x400, HEAP + 0x800
    def word(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    word(unit + 0xb4, kind)
    rng = random.Random(0x40a5f0)
    rows, expected = [], []
    for case in range(16384):
        flags = rng.getrandbits(32)
        if case % 2 == 0:
            flags = (flags | 0x1000000) & ~0x1000
        primary, secondary = rng.getrandbits(32), rng.getrandbits(32)
        constructor, mover, has_mission = [rng.randrange(2) for _ in range(3)]
        mission_flags, capacity = rng.randrange(256), rng.randrange(-32768, 32768)
        category = case % 10
        word(unit + 0x130, flags)
        word(kind + 0x260, primary)
        word(kind + 0x264, secondary)
        word(kind + 0x12e, constructor)
        word(unit + 8, HEAP + 0x1000 if mover else 0)
        word(unit + 0x60, mission if has_mission else 0)
        p.uc.mem_write(mission + 0x5a, bytes([mission_flags]))
        p.uc.mem_write(kind + 0x194, struct.pack('<h', capacity))
        result, error = p.call(0x40a5f0, (unit, category))
        if error:
            raise RuntimeError(error)
        expected.append(result & 255)
        rows.append(' '.join(map(str, [flags, primary, secondary, constructor, mover,
                                      has_mission, mission_flags, capacity, category])))
    result = subprocess.run([args.binary, '--category'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = list(map(int, result.stdout.split()))
    if actual != expected:
        raise AssertionError(next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 16384 original AI squad category checks, including invalid categories and mission filters')


if __name__ == '__main__':
    main()
