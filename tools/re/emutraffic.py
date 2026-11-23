"""Observe traffic scoring and hard footprint placement in KINGDOMS.icd.

Run with python3 tools/re/emutraffic.py. The terrain/list builder is stubbed
to supply one occupant; the speed accessor, fixed multiply and both speed
comparisons execute retail code. No binary code or data is exported.
"""
import struct

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def run(base, own, other, flags=0, road=65536, water=65536, heading=0, other_heading=0):
    icd = Icd()
    uc = icd.uc
    gs, units, nav, typ, head, node = [HEAP + x for x in
                                      (0, 0x20000, 0x30000, 0x40000, 0x50000, 0x50100)]
    me, occupant = units + 312, units + 624

    def w32(a, v):
        uc.mem_write(a, struct.pack('<I', v))

    w32(0x62d55c, gs)
    w32(gs + 0x14e84, units)
    w32(gs + 0x14e88, units + 312 * 3)
    uc.mem_write(0x62dbc4, b'\x01')  # lists already constructed
    w32(0x62dba0, head)
    w32(0x62dbac, 1)
    w32(0x62dbc0, 0)
    w32(head, node)
    uc.mem_write(node + 12, struct.pack('<H', 2))
    w32(me + 8, nav)
    w32(me + 0xb4, typ)
    w32(me + 0x12b, base)
    w32(nav + 0x20, own)
    uc.mem_write(nav + 0x36, struct.pack('<H', flags))
    w32(typ + 0x172, road)
    w32(typ + 0x16e, water)
    w32(occupant + 8, nav + 0x100)
    w32(occupant + 0x130, 0x1000000)
    w32(nav + 0x120, other)
    uc.mem_write(me + 0x7e, struct.pack('<H', heading))
    uc.mem_write(occupant + 0x7e, struct.pack('<H', other_heading))
    icd.hooks[0x507fb0] = lambda uc, args: (5, 6)

    def advance(uc, args):
        w32(uc.reg_read(UC_X86_REG_ECX), head)
        return 0, 0

    icd.hooks[0x4dcc20] = advance
    result, error = icd.call(0x4db640, (me, 8 << 20, 0, 8 << 20))
    assert error is None, error
    return result


if __name__ == '__main__':
    # Traffic uses the unwrapped absolute difference of unsigned retail BAMs.
    for heading, other_heading, expected in ((0, 65535, 2), (65535, 0, 2),
                                             (0, 16384, 6), (0, 16385, 2),
                                             (32767, 32768, 6)):
        grade = run(65536, 65536, 65536, heading=heading, other_heading=other_heading)
        assert grade == expected, (heading, other_heading, grade, expected)
    print('PASS: unsigned traffic heading boundaries')
    # Execute the land local scan with a two-point route. Stub only cell grades;
    # the deadline, scan coordinates, early exit and flag update are retail code.
    for blocked_index in range(-1, 8):
        icd = Icd()
        uc = icd.uc
        gs, settings, state, me, mover, typ, nav = [HEAP + x for x in
            (0, 0x20000, 0x21000, 0x22000, 0x23000, 0x24000, 0x25000)]
        def w32(a, v):
            uc.mem_write(a, struct.pack('<I', v))
        w32(0x62d55c, gs)
        w32(0x62d558, settings)
        w32(settings, state)
        w32(gs + 0x19f44, 666)
        w32(me + 8, mover)
        w32(me + 0xb4, typ)
        uc.mem_write(me + 0x68, struct.pack('<iii', 128*65536, 0, 128*65536))
        uc.mem_write(typ + 0x249, b'\x03')
        uc.mem_write(mover + 0x36, struct.pack('<H', 0x7ed))
        w32(mover, nav)
        w32(nav, 0x5f2a24)
        w32(nav + 0x10c, 2)
        uc.mem_write(nav + 0xc, struct.pack('<4h', 128, 128, 512, 512))
        queries = []
        def grade(uc, args):
            queries.append(struct.unpack('<4i', uc.mem_read(args, 16))[1:])
            return 4, 2 if len(queries)-1 == blocked_index else 6
        icd.hooks[0x4db640] = grade
        _, error = icd.call(0x4dba80, (me,), ecx=mover)
        assert error is None, error
        flags = struct.unpack('<H', uc.mem_read(mover + 0x36, 2))[0]
        assert flags == (13 if blocked_index < 0 else 45), (blocked_index, flags)
        assert struct.unpack('<I', uc.mem_read(mover + 0x30, 4))[0] == 669
        expected = [((128+x*16)*65536, 0, (128+z*16)*65536)
                    for x in (-1, 0, 1) for z in (-1, 0, 1) if x or z]
        assert queries == expected[:8 if blocked_index < 0 else blocked_index+1]
    print('PASS: land local scan coordinates, modes and deadlines')
    # Speeds are 16.16. A faster-than-us occupant still blocks below 75% of
    # our terrain-adjusted base speed, including at one fixed-point unit below.
    for flags, road, water, effective in (
        (0, 65536, 65536, 2 * 65536),
        (0x800, 2 * 65536, 65536, 4 * 65536),
        (0x1000, 65536, 32768, 65536),
        (0x1800, 2 * 65536, 32768, 4 * 65536),
    ):
        floor = effective * 3 // 4
        for own, other, expected in (
            (floor // 2, floor - 1, 2),
            (floor // 2, floor, 6),
            (floor + 1, floor, 2),
            (floor, floor, 6),
        ):
            grade = run(2 * 65536, own, other, flags, road, water)
            print(f'flags={flags:#x} own={own} occupant={other}: {grade}')
            assert grade == expected, (grade, expected)

    # The actual movement commit calls this placement test at 0x4daf8f.
    # Unlike the traffic query, it rejects moving occupants too, including an
    # occupant under a non-origin cell of a 2x2 footprint. No routines stubbed.
    from emupath import World

    icd = Icd()
    world = World(icd)
    typ = world.mktype(0)
    icd.uc.mem_write(typ + 0x126, struct.pack('<hh', 2, 2))
    icd.uc.mem_write(typ + 0x24a, b'\x01')
    for moving in (False, True):
        world.mkunit(2, 10, 10, typ, moving=moving)
        world.occupy(10, 10, 2)
        for x, z, expected in ((11, 11, 1), (10, 10, 0), (9, 9, 0), (8, 8, 1)):
            result, error = icd.call(0x507d10, (typ, 1, x | (z << 16), 1, 0))
            print(f'placement moving={moving} origin=({x},{z}): {result}')
            assert error is None, error
            assert result == expected, (result, expected)
