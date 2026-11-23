#!/usr/bin/env python3
"""Drive the icd's own path search (0x4146e0) over a synthetic grid and print the
cells it walks, so our port can be diffed against it step for step.

The search object is a struct we build ourselves; offsets are the ones the
disassembly uses. Sub-calls that would need the live game are hooked:
  0x4139d0  per-cell query   -> our synthetic grid
  0x413e50  distance to goal -> chebyshev
"""
import struct
import sys
from emu import Icd, HEAP
from unicorn.x86_const import *

W, H = 24, 12
GRID = [
    "........................",
    "........#...............",
    "........#...............",
    "........#...............",
    "........#...............",
    "........#...............",
    "........#...............",   # wall across the path...
    "........................",   # ...with a gap here
    "........#...............",
    "........#...............",
    "........................",
    "........................",
]
START = (1, 6)
GOAL = (20, 6)

OBJ = HEAP + 0x1000          # the search object
CELLS = HEAP + 0x8000        # 4 bytes per cell (flags, dir, ...)
BITS = HEAP + 0x20000        # visited bitmap


def score(cx, cz):
    if cx < 0 or cz < 0 or cx >= W or cz >= H:
        return 0
    return 0 if GRID[cz][cx] == '#' else 6


def main():
    icd = Icd()
    uc = icd.uc
    calls = []

    probes = []
    def q_cell(uc, argp):
        # (this, x, z, dir) -- stdcall args after the return address
        x, z, d = struct.unpack("<iii", uc.mem_read(argp, 12))
        probes.append((x, z, d, score(x, z)))
        return 3, score(x, z)

    def q_dist(uc, argp):
        x, z = struct.unpack("<ii", uc.mem_read(argp, 8))
        return 2, max(abs(x - GOAL[0]), abs(z - GOAL[1]))

    icd.hooks[0x4139d0] = q_cell
    icd.hooks[0x413e50] = q_dist

    # trace every write of the two cursors so we can see the walk
    walk = []

    def watch(uc, addr, size, _):
        if addr == 0x414d2e:      # cursor A committed  (+0xf8/+0xfc)
            ax = uc.reg_read(UC_X86_REG_EDI); az = uc.reg_read(UC_X86_REG_EBX)
            walk.append(("A", ax, az))
        elif addr == 0x414efb:    # cursor B committed  (+0x100/+0x104)
            bx = uc.reg_read(UC_X86_REG_EDI); bz = uc.reg_read(UC_X86_REG_EBX)
            walk.append(("B", bx, bz))
        elif addr == 0x414900:    # octant march committed (+0xd0/+0xd4)
            mx = uc.reg_read(UC_X86_REG_EDI); mz = uc.reg_read(UC_X86_REG_EBX)
            walk.append(("m", mx, mz))
        elif addr == 0x414b48:    # cardinal march committed
            walk.append(("c", uc.reg_read(UC_X86_REG_EDI),
                         struct.unpack("<i", uc.mem_read(uc.reg_read(UC_X86_REG_EBP) - 0xc, 4))[0]))

    from unicorn import UC_HOOK_CODE
    uc.hook_add(UC_HOOK_CODE, watch)

    # A stand-in for the game-state struct the search reads its map size from
    # (0x62d55c is the pointer; +0x19e98 / +0x19e9c are width and height).
    GS = HEAP + 0x100000
    uc.mem_write(GS, b"\0" * 0x20000)
    uc.mem_write(0x62d55c, struct.pack("<I", GS))
    uc.mem_write(GS + 0x19e98, struct.pack("<i", W))
    uc.mem_write(GS + 0x19e9c, struct.pack("<i", H))

    # zero the object and the scratch, then fill the fields the search reads
    uc.mem_write(OBJ, b"\0" * 0x400)
    uc.mem_write(CELLS, b"\0" * (W * H * 4))
    uc.mem_write(BITS, b"\0" * (W * H // 8 + 64))

    def w32(off, v):
        uc.mem_write(OBJ + off, struct.pack("<i", v))

    def w16(off, v):
        uc.mem_write(OBJ + off, struct.pack("<h", v))

    w32(0x1c, CELLS)          # per-cell records
    w32(0x20, W)              # map width
    w32(0x2c, BITS)           # visited bitmap
    w16(0x30, START[0]); w16(0x32, START[1])
    w32(0x38, GOAL[0]);  w32(0x3c, GOAL[1])
    w32(0x48, 0)              # work
    w32(0x60, 0)              # state
    w32(0x165, 100000)        # work cap: one big quantum
    w32(0xe8, (W + H) * 20)   # visit limit

    print("grid %dx%d, start %s goal %s" % (W, H, START, GOAL))
    for row in GRID:
        print("   " + row)
    print()

    for step in range(400):
        eax, err = icd.call(0x4146e0, args=(), ecx=OBJ)
        if err:
            print("emulation stopped:", err)
            break
        state = struct.unpack("<i", uc.mem_read(OBJ + 0x60, 4))[0]
        visited = struct.unpack("<i", uc.mem_read(OBJ + 0xe4, 4))[0]
        r = struct.unpack("<i", struct.pack("<I", eax))[0]
        if r == 0:
            print("ARRIVED after %d calls, state=%d visited=%d" % (step + 1, state, visited))
            break
        if r == -2:
            print("suspended (out of quantum) state=%d visited=%d" % (state, visited))
            w32(0x165, struct.unpack("<i", uc.mem_read(OBJ + 0x165, 4))[0] + 100000)
            continue
        # -1: emitted a waypoint
    else:
        print("did not finish in 400 calls")

    print("\nprobe order (x,z,dir -> score):")
    for i, (x, z, d, sc) in enumerate(probes[:40]):
        print("   %2d: (%2d,%2d) dir=%d -> %d%s" % (i, x, z, d, sc, "  ACCEPT" if sc >= 4 else ""))

    print("\nwalk (%d steps):" % len(walk))
    line = []
    for kind, x, z in walk[:120]:
        line.append("%s(%d,%d)" % (kind, x, z))
    print("  " + " ".join(line))


if __name__ == "__main__":
    main()
