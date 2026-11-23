#!/usr/bin/env python3
"""Compare projected map-edge markers with the complete retail 50eef0 routine."""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runner', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd()
    game, cells, scenario = HEAP, HEAP+0x20000, HEAP+0x30000
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v))
    put(0x62d55c, game)
    put(game+0x19f04, cells)
    put(game+0x175dc, scenario)
    p.freeze_hooks()
    rng = random.Random(0x50eef0)
    rows, expected = [], []
    for case in range(300):
        w, h = rng.randint(2, 24), rng.randint(20, 32)
        sea, water = rng.randrange(256), case % 2
        put(game+0x19e88, w*16)
        put(game+0x19e8c, h*16)
        put(game+0x19e98, w)
        put(game+0x19e9c, h)
        put(scenario+0xd39, water)
        p.uc.mem_write(game+0x19ef8, bytes([sea]))
        raw = bytearray(w*h*14)
        row = [w, h, h*16, sea, water]
        for i in range(w*h):
            feature = rng.choice((0xffff, 0xffff, 0xfffe, 0xfffd, 0xfffc, 0xfffb, 0, 7))
            height, low = rng.randrange(256), rng.randrange(256)
            raw[i*14+4] = height
            raw[i*14+6] = low
            struct.pack_into('<H', raw, i*14+8, feature)
            row.extend((feature, height, low))
        p.uc.mem_write(cells, bytes(raw))
        _, error = p.call(0x50eef0)
        if error:
            raise AssertionError((case, error))
        actual = bytes(p.uc.mem_read(cells, len(raw)))
        expected.append([struct.unpack_from('<H', actual, i*14+8)[0] for i in range(w*h)])
        rows.append(' '.join(map(str, row)))
    result = subprocess.run([args.runner, '--map-boundary'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(expected):
        raise AssertionError('wrong result count')
    for i, (line, want) in enumerate(zip(lines, expected)):
        got = list(map(int, line.split()))
        if got != want:
            raise AssertionError((i, 'boundary plane differs'))
    print('PASS: 300 complete map-boundary planes, original executable')


if __name__ == '__main__':
    main()
