#!/usr/bin/env python3
"""How far does retail lift a unit up-screen for terrain height?

0x511140 takes a point (x,z as int16), looks its cell up through 0x50e600 and
returns one byte out of the cell record. Its caller (0x426820, the navigator's
own debug draw) then does:

    screenY = (z << 4) - cameraY - (thatByte >> 1)
    screenX = (x << 4) - cameraX            <- no height term at all

So the lift is height/2 in Y and nothing in X. This drives 0x511140 with a
stubbed cell lookup to confirm WHICH byte of the record it reads, since that is
the one assumption the reading rests on.

    pip install unicorn && python3 tools/re/emulift.py
"""
import struct

from emu import Icd, HEAP
from unicorn.x86_const import *

CELL = HEAP + 0x2000
PT = HEAP + 0x1000


def main():
    icd = Icd()
    uc = icd.uc

    asked = []

    def cell_lookup(uc, argp):          # 0x50e600(x, z) -> cell record*
        x, z = struct.unpack("<ii", uc.mem_read(argp, 8))
        asked.append((x, z))
        return 2, CELL

    icd.hooks[0x50E600] = cell_lookup

    # A cell record with a different value in every early byte, so whichever one
    # comes back identifies itself.
    uc.mem_write(CELL, bytes(range(0, 32)))

    print("cell record bytes: +0=%d +1=%d +2=%d +3=%d +4=%d +5=%d +6=%d\n"
          % tuple(range(7)))

    for (px, pz) in ((10, 20), (100, 7), (-3, 64)):
        uc.mem_write(PT, struct.pack("<hh", px, pz))
        eax, err = icd.call(0x511140, args=(PT,))
        if err:
            print("  emulation stopped:", err)
            return
        print("  point (%4d,%4d) -> lookup asked for %s, returned %d"
              % (px, pz, asked[-1], eax))

    print("\n  => it returns byte +4 of the cell record: the terrain height.")

    # And with a real height, what does the caller's arithmetic give?
    print("\nlift the caller applies (screenY = z*16 - camY - height/2):")
    for h in (0, 20, 40, 60, 80, 120, 160, 255):
        uc.mem_write(CELL + 4, bytes([h]))
        uc.mem_write(PT, struct.pack("<hh", 10, 20))
        eax, _ = icd.call(0x511140, args=(PT,))
        print("   height %3d -> byte %3d -> %5.1f px up-screen   (ours at 1.1: %5.1f)"
              % (h, eax, eax / 2.0, h * 1.1))


if __name__ == "__main__":
    main()
