#!/usr/bin/env python3
"""Compare complete wind updates with retail; stub only CRT thread lookup."""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_motion_test')
    parser.add_argument('--count', type=int, default=4000)
    args = parser.parse_args()
    icd = Icd()
    game, thread = HEAP, HEAP + 0x20000
    def put(address, value):
        icd.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    def get(address):
        return struct.unpack('<I', icd.uc.mem_read(address, 4))[0]
    put(0x62d55c, game)
    icd.hooks[0x5dc403] = lambda uc, sp: (0, thread)
    icd.freeze_hooks()
    rng = random.Random(525170)
    rows, expected = [], []
    for i in range(args.count):
        minimum = rng.randrange(0, 5001)
        maximum = minimum + rng.randrange(0, 5001)
        if i % 7 == 0: minimum = maximum = 0
        tick = rng.randrange(1, 0xfffffff0)
        deadline = [tick-1, tick, tick+1][i % 3]
        x, z, speed = (rng.randrange(-10000, 10001) for _ in range(3))
        heading, flags = rng.randrange(65536), rng.randrange(65536)
        seed, crt = rng.randrange(1, 0x7fffffff), rng.randrange(0x100000000)
        rows.append(f'w {minimum} {maximum} {tick} {deadline} {x} {z} {speed} {heading} {flags} {seed} {crt}')
        for offset, value in ((0x19ec4,minimum),(0x19ec8,maximum),(0x19f44,tick),
                              (0x19f58,deadline),(0x19f60,x),(0x19f68,z),(0x19f6c,speed),
                              (0x19f70,heading | flags << 16)):
            put(game+offset,value)
        put(0x64186c, seed)
        put(thread+0x14, crt)
        _, error = icd.call(0x525170)
        if error: raise RuntimeError(error)
        signed = lambda offset: struct.unpack('<i', icd.uc.mem_read(game+offset,4))[0]
        result = [get(game+0x19f58),signed(0x19f60),signed(0x19f68),signed(0x19f6c),
                  get(game+0x19f70)&65535,get(game+0x19f70)>>16,get(0x64186c),get(thread+0x14)]
        expected.append(result)
    proc = subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,
                          capture_output=True,check=True)
    actual = [list(map(int,row.split())) for row in proc.stdout.splitlines()]
    if len(actual) != len(expected): raise AssertionError('oracle row count differs')
    for row, want, got in zip(rows,expected,actual):
        if want != got: raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} wind updates, deadlines, flags and both RNG states')


if __name__ == '__main__':
    main()
