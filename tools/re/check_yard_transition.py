#!/usr/bin/env python3
"""Observe 507c70 yard writes, occupancy vetoes and ordered cache callbacks.

The original occupancy check executes. Restamping and cache refresh are observed
at their call boundaries; this test does not substitute the transition itself.
"""
import itertools
import struct

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    p = Icd()
    game, cells, pool, kind = (HEAP + n for n in (0, 0x20000, 0x30000, 0x40000))
    unit, body, yard = pool + 312, pool + 624, kind + 0x1000

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value))

    put(0x62d55c, game)
    for offset, value in ((0x19e98, 16), (0x19e9c, 16), (0x19f04, cells),
                          (0x14e84, pool), (0x14e88, body)):
        put(game + offset, value)
    put(unit + 0xb4, kind)
    put(kind + 0x12a, yard)
    put(unit + 0x130, 0x1000001)
    put(body + 0x130, 0x1000001)
    p.uc.mem_write(unit + 0x74, struct.pack('<4h', 6, 6, 1, 1))
    events = []

    def stamp(uc, args):
        assert struct.unpack('<I', uc.mem_read(args, 4))[0] == unit
        events.append('stamp')
        return 1, 0

    def refresh(uc, args):
        assert struct.unpack('<2I', uc.mem_read(args, 8)) == (6 | (6 << 16), 1 | (1 << 16))
        events.append('refresh')
        return 2, 0

    p.hooks[0x5062d0] = stamp
    p.hooks[0x4e1e20] = refresh
    count = 0
    for state, requested, yardbits, occupant in itertools.product(
            (0, 4), (0, 1, 2, 3), range(8), (0, 1, 2)):
        p.uc.mem_write(unit + 0x12f, bytes([state]))
        p.uc.mem_write(yard, bytes([yardbits]))
        p.uc.mem_write(cells + (6 * 16 + 6) * 14, struct.pack('<H', occupant))
        events.clear()
        _, error = p.call(0x507c70, (unit, requested))
        assert error is None, error
        assert p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000
        accepted = occupant != 2 or not (yardbits & (2 if requested else 4))
        assert events == (['stamp', 'refresh'] if accepted else []), (
            state, requested, yardbits, occupant, events)
        expected = (requested & 1) * 4 if accepted else state
        assert bytes(p.uc.mem_read(unit + 0x12f, 1))[0] == expected
        count += 1
    print(f'PASS: {count} native yard writes, occupancy vetoes and ordered cache refresh callbacks')


if __name__ == '__main__':
    main()
