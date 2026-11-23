#!/usr/bin/env python3
"""Compare occupied-base factory admission and its random deferral decisions."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_FPCW


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_ai_test')
    args = ap.parse_args()
    p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, squad, manager, owner, resource, unit, mission, types = [HEAP+i*0x30000 for i in range(8)]
    def word(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    word(0x62d55c, game); word(game+0x175c4, types)
    word(squad+4, manager); word(manager, owner); word(owner+0x10c, resource)
    p.uc.mem_write(mission+4, b'\x01')
    choice, selections, output = 0, 0, 0
    def select(uc, argv):
        nonlocal selections
        selections += 1
        return 3, choice
    def identify(uc, argv):
        p.uc.mem_write(uc.reg_read(UC_X86_REG_ECX), b'\x01')
        return 1, 0
    def queue(uc, argv):
        nonlocal output
        output = (get(argv)-types-32)//676
        return 3, 0
    p.hooks[0x412d00] = select
    p.hooks[0x4d4bf0] = identify
    p.hooks[0x429810] = queue
    p.freeze_hooks()
    calls = []
    def observe(uc, address, size, data):
        calls.append(struct.unpack('<i', uc.mem_read(uc.reg_read(UC_X86_REG_ESP)+4, 4))[0])
    p.uc.hook_add(UC_HOOK_CODE, observe, begin=0x535cc0, end=0x535cc0)
    rng = random.Random(0x40dcb0)
    rows, expected = [], []
    for case in range(8192):
        allocation = rng.choice([0., .2333333, .23333334, .2333334, .6999999, .7, .7000001, 1.])
        allocation = struct.unpack('<f', struct.pack('<f', allocation))[0]
        queued, population, choice = rng.randrange(2), rng.randrange(80), rng.randrange(2)
        cost = rng.choice([0., 499., 500., 500.01, 799.999, 800., 2000., 1e9])
        cost = struct.unpack('<f', struct.pack('<f', cost))[0]
        seed = rng.getrandbits(32)
        p.uc.mem_write(resource+8, struct.pack('<f', allocation))
        p.uc.mem_write(owner+0xe8, struct.pack('<H', population))
        p.uc.mem_write(types+676+0x20e, struct.pack('<f', cost))
        word(unit+0x60, mission if queued else 0)
        word(0x64186c, seed)
        selections, output = 0, 0; calls.clear()
        _, error = p.call(0x40dcb0, (unit,), ecx=squad)
        if error: raise RuntimeError(error)
        expected.append([output, selections, get(0x64186c), len(calls), *calls])
        rows.append(f'{allocation} {queued} {population} {choice} {cost} {seed}')
    result = subprocess.run([args.binary, '--factory-plan'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [list(map(int, row.split())) for row in result.stdout.splitlines()]
    if actual != expected:
        raise AssertionError(next(((rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                  (len(actual), len(expected))))
    print('PASS: 8192 original factory planning decisions, allocation thresholds, selections and RNG order')


if __name__ == '__main__':
    main()
