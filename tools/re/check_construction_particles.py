#!/usr/bin/env python3
"""Compare construction particle capacity, CRT draws, motion and expiry.

The original CRT and trig routines execute. Allocation and sprite animation
are controlled hosts; animation has no effect on particle lifetime.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_construction_test')
    args = ap.parse_args()
    p = Icd()
    emitter, owner, head, thread, nodes = [HEAP+n*0x10000 for n in range(5)]
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    word = lambda a: struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    allocated = []
    def allocate(uc, sp):
        address = nodes+len(allocated)*44
        allocated.append(address)
        return 1, address
    p.hooks[0x4f3840] = allocate
    p.hooks[0x5dc403] = lambda uc, sp: (0, thread)
    p.hooks[0x537390] = lambda uc, sp: (3, 0)
    p.hooks[0x5373d0] = lambda uc, sp: (1, 0)
    p.freeze_hooks()
    rng = random.Random(0x4f1430); rows = []; expected = []
    for case in range(4096):
        capacity, count = rng.randrange(9), rng.randrange(12)
        radius = rng.randrange(1, 80)*65536
        height = rng.randrange(0, 200)*65536
        owner_height = rng.randrange(-50, 100)*65536
        rising, seed, steps = rng.randrange(2), rng.getrandbits(32), rng.randrange(150)
        allocated.clear()
        put(emitter+8, head); put(head, head); put(head+4, head)
        put(emitter+12, 0); put(emitter+16, capacity); put(emitter+20, owner)
        put(emitter+24, radius); put(emitter+28, height)
        put(owner+0x6c, owner_height); put(thread+0x14, seed)
        _, error = p.call(0x4f1430, (count, 0, rising), ecx=emitter)
        if error: raise RuntimeError(error)
        assert word(emitter+12) == min(capacity, count)
        for step in range(steps):
            _, error = p.call(0x4f32d0, ecx=emitter)
            if error: raise RuntimeError(error)
        live = []; node = word(head)
        while node != head:
            if len(live) >= capacity: raise AssertionError('invalid particle list')
            live.append(node+8); node = word(node)
        assert len(live) == word(emitter+12)
        values = [word(thread+0x14), len(live)]
        for particle in live:
            values.extend(struct.unpack('<5i', p.uc.mem_read(particle+4, 20)))
        expected.append(values)
        rows.append(' '.join(map(str, [capacity, count, radius, height, owner_height, rising, seed, steps])))
    proc = subprocess.run([args.binary, '--particles'], input='\n'.join(rows)+'\n',
                          text=True, capture_output=True, check=True)
    actual = [list(map(int, row.split())) for row in proc.stdout.splitlines()]
    if actual != expected:
        for row, want, got in zip(rows, expected, actual):
            if want != got: raise AssertionError((row, want, got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} construction particle emitters, CRT seeds, motion and expiry')


if __name__ == '__main__': main()
