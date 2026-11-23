#!/usr/bin/env python3
"""Check empty-target AI patrol sampling against the original executable.

Only the placement host is substituted. Bounds, attempts, separation, trig and
random draws execute in the retail routine; terrain placement is a separate check.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_ESP,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = parser.parse_args()
    p = Icd()
    unit, kind, game = HEAP, HEAP + 4096, HEAP + 8192
    bp = STACK + STACK_SZ - 4096

    def put(address, *values):
        p.uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                          *(v & 0xffffffff for v in values)))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    counters = {}

    def placement(uc, address):
        accepted = counters['queries'] >= counters['rejections']
        counters['queries'] += 1
        return 5, int(accepted)

    def observe(uc, address, size, unused):
        if address == 0x535cc0:
            counters['draws'] += 1
        else:
            counters['success'] = address == 0x40db3f
            uc.emu_stop()

    p.hooks[0x507d10] = placement
    # Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
    p.allow_early_stop = True
    p.freeze_hooks()
    for address in (0x535cc0, 0x40db3f, 0x40d732):
        p.uc.hook_add(UC_HOOK_CODE, observe, begin=address, end=address)
    put(0x62d55c, game)
    put(unit + 0xb4, kind)
    put(unit + 0x78, 0x00020002)
    rng = random.Random(0x40d7ad)
    fixtures, expected = [], []
    for i in range(4000):
        width, height = rng.randint(128, 16000), rng.randint(128, 16000)
        radius = rng.choice((1, 2, 50, 100, 560, 2000))
        x, z = rng.randrange(width * 65536), rng.randrange(height * 65536)
        seed = rng.randrange(2**32)
        rejections = rng.choice((0, 1, 3, 9, 10, 20))
        counters.update(draws=0, queries=0, rejections=rejections, success=None)
        p.uc.mem_write(bp - 256, bytes(512))
        put(bp + 8, unit)
        put(bp - 36, x, 0, z)
        put(bp - 8, radius)
        put(game + 0x19e88, width, height)
        put(0x64186c, seed)
        for reg, value in ((UC_X86_REG_EBP, bp), (UC_X86_REG_ESP, bp - 512),
                           (UC_X86_REG_EAX, 0), (UC_X86_REG_ECX, 0)):
            p.uc.reg_write(reg, value)
        p.uc.emu_start(0x40d7ad, 0x40db40, count=100000)
        if counters['success'] is None:
            raise AssertionError('native sampler did not terminate')
        row = (int(counters['success']), get(0x64186c),
               counters['draws'], counters['queries'])
        if counters['success']:
            first = struct.unpack('<3i', p.uc.mem_read(bp - 104, 12))
            second = struct.unpack('<3i', p.uc.mem_read(bp - 92, 12))
            row += (first[0], first[2], second[0], second[2])
        expected.append(row)
        fixtures.append(' '.join(map(str, (x, z, radius, width, height, seed, rejections))))
    output = subprocess.run([args.binary, '--patrol-points'],
                            input='\n'.join(fixtures) + '\n', text=True,
                            capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in output.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('oracle row count mismatch')
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError((i, fixtures[i], got, want))
    print(f'PASS: {len(fixtures)} AI patrol samples, placement counts and RNG states')


if __name__ == '__main__':
    main()
