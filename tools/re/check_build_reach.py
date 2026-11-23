#!/usr/bin/env python3
"""Compare failed-build reach decisions with the original MobileBuild handler.

The handler executes through its reach decision. The harness observes the
placement boundary, and substitutes only the out-of-range UI notification.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-dbg/retail_construction_test')
    args = parser.parse_args()
    p = Icd()
    game, unit, mission, mover_type, site = [HEAP+n*0x20000 for n in range(5)]
    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    put(0x62d55c, game)
    put(game+0x175c4, site-676)
    put(unit+8, HEAP+0xa0000)
    put(unit+0xb4, mover_type)
    put(mission+0xe, unit)
    put(mission+0x4e, 1)
    p.uc.mem_write(mission+5, b'\x01')
    placement = False
    def reached(uc, address, size, data):
        nonlocal placement
        placement = True
        uc.reg_write(UC_X86_REG_EIP, 0x6ffff000)
    p.uc.hook_add(UC_HOOK_CODE, reached, begin=0x405824, end=0x405824)
    notices = 0
    def notice(uc, argv):
        nonlocal notices
        assert struct.unpack('<2I', uc.mem_read(argv, 8)) == (unit, 0x604ed4)
        notices += 1
        return 2, 0
    p.hooks[0x4f5db0] = notice
    p.freeze_hooks()
    rng = random.Random(0x405756)
    rows, expected = [], []
    for case in range(8192):
        fx, fz, sx, sz = [rng.randrange(1, 33) for _ in range(4)]
        reach = rng.randrange(513)
        gx, gz = [rng.randrange(1024*65536, 4096*65536) for _ in range(2)]
        if case % 2:
            # Concentrate on the transition, including fractional positions.
            edge = int((fx*fx+fz*fz)**0.5*8)+int((sx*sx+sz*sz)**0.5*8)
            x = gx+(reach+edge+rng.randrange(-2, 3))*65536+rng.randrange(65536)
            z = gz
        else:
            x, z = [rng.randrange(8192*65536) for _ in range(2)]
        put(unit+0x68, x); put(unit+0x70, z)
        put(mission+0x22, gx); put(mission+0x2a, gz)
        p.uc.mem_write(unit+0x78, struct.pack('<2h', fx, fz))
        p.uc.mem_write(site+0x126, struct.pack('<2h', sx, sz))
        p.uc.mem_write(mover_type+0x230, struct.pack('<H', reach))
        placement = False
        before = notices
        value, error = p.call(0x405560, (unit, mission, 0x200))
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
        assert placement or (value == 8 and notices == before+1)
        rows.append(' '.join(map(str, (x, z, gx, gz, fx, fz, sx, sz, reach))))
        expected.append(int(placement))
    result = subprocess.run([args.binary, '--build-reach'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = list(map(int, result.stdout.split()))
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, wanted, got in zip(rows, expected, actual):
        assert wanted == got, (row, wanted, got)
    assert 0 < sum(expected) < len(expected)
    print(f'PASS: {len(rows)} original failed-build reach decisions, including threshold and fractional-position cases')


if __name__ == '__main__':
    main()
