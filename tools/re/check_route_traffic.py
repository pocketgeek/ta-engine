#!/usr/bin/env python3
"""Check origin-relative traffic flags in retail tracing and reconstruction.

Uses controlled grades and a planted straight parent chain. It verifies flag
semantics, not the legacy port's route selection or full mission timing.
"""
import struct

from emuphase import Phase, OBJ


def main():
    for traffic, flags in (((9,), 1), ((25,), 2), ((9, 10), 3), ((9, 25), 3), ((16,), 2)):
        p = Phase(40, 24)
        unit = p.unit(8, 12)
        assert p.construct() is None
        p.plant_request(unit, (8, 12), (26, 12))
        assert p.init()[1] is None
        def grade(uc, args):
            x, z = struct.unpack('<ii', uc.mem_read(args, 8))
            return 3, (5 if z == 12 and x in traffic else 6) if 0 <= x < 40 and 0 <= z < 24 else 0
        p.icd.hooks[0x4139d0] = grade
        p.uc.mem_write(OBJ + 0x165, struct.pack('<I', 12000))
        _, error = p.icd.call(0x4146e0, ecx=OBJ)
        assert error is None, error
        actual = int(bool(p.get(0x4c))) | int(bool(p.get(0x50))) << 1
        assert actual == flags, ('visits', traffic, actual, flags)
        # A separate parent chain isolates reconstruction from route choice.
        for x in range(9, 27):
            p.uc.mem_write(p.get(0x1c) + (12*40+x)*4,
                           bytes((8 | (64 if x in traffic else 0), 6, 0, 0)))
        p.uc.mem_write(OBJ + 0x34, struct.pack('<hh', 26, 12))
        p.uc.mem_write(OBJ + 0x40, struct.pack('<I', 0))
        p.icd.hooks[0x4e4ea0] = lambda uc, args: (2, 0)
        _, error = p.icd.call(0x414450, (0,), ecx=OBJ)
        assert error is None, error
        actual = struct.unpack('<I', p.uc.mem_read(unit + 0x134, 4))[0] & 3
        assert actual == flags, ('reconstruction', traffic, actual, flags)
    print('PASS: five retail trace/reconstruction traffic fixtures, including strict radius boundary and repeated near traffic')


if __name__ == '__main__':
    main()
