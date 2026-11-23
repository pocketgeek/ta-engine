#!/usr/bin/env python3
"""Compare resource-history rolling and the original priority pressure branch."""
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
    p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, owner, manager, resource, types, arrays = [HEAP+n*0x30000 for n in range(6)]
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v))
    real = lambda a, v: p.uc.mem_write(a, struct.pack('<f', v))
    put(0x62d55c, game); put(game+0x175b8, 2); put(game+0x175c4, types)
    put(manager, owner); put(owner+0x10c, resource)
    for offset, address in ((0x65, arrays), (0xa1, arrays+0x100)):
        put(manager+offset+4, address);put(manager+offset+8, address+2);put(manager+offset+12, address+2)
    for offset, address in ((0x85, arrays+0x200), (0x95, arrays+0x300), (0xe5, arrays+0x400)):
        put(manager+offset, address)
    put(types+676+0x260, 0x100)
    p.uc.mem_write(types+676+0x194, struct.pack('<h', -1))
    p.hooks[0x4107e0] = lambda uc, sp: (1, 0)
    p.freeze_hooks()
    rng = random.Random(0x401270); rows = []; expected = []
    for case in range(4096):
        f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
        stored = rng.choice([0., 0.009, 0.01, 0.011, 100.])
        pending = [f32(rng.random()*10) for _ in range(2)]
        samples = [[f32(rng.random()*10) for _ in range(2)] for _ in range(30)]
        if case % 3 == 0:
            # Exercise both sides of the strict +/- .01 pressure thresholds.
            samples = [[0., 0.] for _ in range(30)]
            samples[1][case % 2] = f32(rng.choice([0., 0.009, 0.01, 0.011])/1.034482717514038)
        real(resource, stored); real(resource+4, 1000.)
        real(resource+12, pending[0]); real(resource+16, pending[1])
        for i, sample in enumerate(samples):
            real(resource+0x2c+12*i, sample[0]); real(resource+0x30+12*i, sample[1])
        _, error = p.call(0x401270, ecx=resource)
        if error: raise RuntimeError(error)
        _, error = p.call(0x410810, ecx=manager)
        if error: raise RuntimeError(error)
        score = p.uc.mem_read(arrays+1, 1)[0]
        if score not in (24, 64): raise AssertionError(score)
        words = []
        for i in range(30): words.extend(struct.unpack('<2I', p.uc.mem_read(resource+0x2c+12*i, 8)))
        expected.append([int(score == 24), *words])
        rows.append(' '.join(map(str, [stored, *pending, *sum(samples, [])])))
    result = subprocess.run([args.binary, '--resources'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in result.stdout.splitlines()]
    if actual != expected:
        for row, want, got in zip(rows, expected, actual):
            if want != got: raise AssertionError((row, want, got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} resource histories, sample exclusion and pressure thresholds')


if __name__ == '__main__': main()
