#!/usr/bin/env python3
"""Compare construction income/allocation and end-of-tick clamping with retail."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_EBP, UC_X86_REG_EDX, UC_X86_REG_EDI


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_construction_test')
    args = ap.parse_args()
    p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    owner, resource = HEAP, HEAP+0x10000
    frame = STACK+STACK_SZ-0x2000
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v))
    real = lambda a, v: p.uc.mem_write(a, struct.pack('<f', v))
    double = lambda a, v: p.uc.mem_write(a, struct.pack('<d', v))
    word = lambda a: struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    wide = lambda a: struct.unpack('<Q', p.uc.mem_read(a, 8))[0]
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
    put(owner+0x10c, resource)
    rng = random.Random(0x51d837); rows = []; expected = []
    for case in range(8192):
        stored = f32(rng.choice([0, -1, 1, rng.uniform(-10, 10000)]))
        override = f32(rng.choice([0, -1, 1, 100, rng.random()*10000]))
        produced = f32(rng.random()*100)
        total, excess = rng.random()*1e6, rng.random()*1e4
        income = f32(rng.choice([0, 31, 47, rng.random()*1000]))
        storage = f32(rng.choice([0, 1, -1, rng.random()*10000]))
        demand = f32(rng.choice([0, 1, 42.41, rng.random()*10000]))
        # Zero demand with negative storage divides by zero in the original;
        # it is outside the finite resource-state contract being compared.
        if stored < 0 and demand == 0: demand = 1
        real(resource, stored); real(resource+0x14, override)
        real(resource+0xc, produced); double(resource+0x18, total); double(resource+0x20, excess)
        real(frame-8, income); real(frame-12, storage); real(frame-24, demand)
        p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_EDX, owner)
        p.uc.reg_write(UC_X86_REG_EDI, 0x3f800000)
        # Execute the original aggregate accounting block, stopping before the
        # next player. Unit eligibility and income aggregation are host inputs.
        p.uc.emu_start(0x51d837, 0x51d8c4, timeout=5_000_000)
        before = [word(resource), word(resource+4), word(resource+8), word(resource+12), wide(resource+24)]
        _, error = p.call(0x401270, ecx=resource)
        if error: raise RuntimeError(error)
        expected.append(before+[word(resource), word(resource+4), wide(resource+32)])
        rows.append(' '.join(map(str, [stored, override, produced, total, excess, income, storage, demand])))
    proc = subprocess.run([args.binary, '--economy'], input='\n'.join(rows)+'\n',
                          text=True, capture_output=True, check=True)
    actual = [list(map(int, row.split())) for row in proc.stdout.splitlines()]
    if actual != expected:
        for row, want, got in zip(rows, expected, actual):
            if want != got: raise AssertionError((row, want, got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} construction income, allocation and resource clamp cases')


if __name__ == '__main__': main()
