#!/usr/bin/env python3
"""Decide, empirically, how retail stores a numeric field.

Asked while converting the last of our sim floats to fixed point: for each
field, is it a float, a 16.16 fixed-point value, or an integer count of ticks?
Reading the load opcode answers it in principle (`flds`/`fldl` load a float,
`fildl` loads an integer), but the SCALE the engine multiplies by afterwards is
what says which KIND of integer -- and that is worth confirming by running it
rather than by eye.

Two scales appear all over the binary, both as .rdata doubles:

    0x5f25d8 = 0.0333... = 1/30      -> the field is a count of 30Hz TICKS
    0x5eba78 = 1.5258...e-05 = 1/65536 -> the field is 16.16 FIXED POINT

This plants known values in a synthetic struct, runs the engine's own display
sequences over them, and prints what comes out. Observation only: nothing from
the binary is copied into the engine, and it reads YOUR OWN retail install from
the gitignored assets/ directory at run time.

    pip install unicorn
    python3 tools/re/emufields.py
"""
import struct
from unicorn import *
from unicorn.x86_const import *
from emu import Icd, STACK, STACK_SZ

STRUCT = 0x72000000
CODE = 0x73000000

# Lifted verbatim from the two display paths.
#
# 0x4fbfb2, "Reload Time = %f" (the weapon dump at 0x617770):
#   xor %ecx,%ecx ; mov 0x9c(%esi),%cx ; mov %ecx,-0x18(%ebp)
#   fildl -0x18(%ebp) ; fmull 0x5f25d8 ; fstpl (%esp)
RELOAD = bytes([
    0x33, 0xC9,
    0x66, 0x8B, 0x8E, 0x9C, 0x00, 0x00, 0x00,
    0x89, 0x4D, 0xE8,
    0xDB, 0x45, 0xE8,
    0xDC, 0x0D, 0xD8, 0x25, 0x5F, 0x00,
    0xDD, 0x1C, 0x24,
])
# 0x4fbe6c, the same dump's 16.16 fields:
#   fildl 0x172(%esi) ; fmull 0x5eba78 ; fstpl (%esp)
FIXED = bytes([
    0xDB, 0x86, 0x72, 0x01, 0x00, 0x00,
    0xDC, 0x0D, 0x78, 0xBA, 0x5E, 0x00,
    0xDD, 0x1C, 0x24,
])


def run(icd, code, off, raw, width):
    uc = icd.uc
    uc.mem_write(CODE, code)
    uc.mem_write(STRUCT + off, struct.pack("<i" if width == 4 else "<h", raw))
    sp = STACK + STACK_SZ - 0x2000
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EBP, sp + 0x200)
    uc.reg_write(UC_X86_REG_ESI, STRUCT)
    uc.emu_start(CODE, CODE + len(code))
    return struct.unpack("<d", uc.mem_read(sp, 8))[0]


def main():
    icd = Icd()
    icd.uc.mem_map(STRUCT, 0x1000)
    icd.uc.mem_map(CODE, 0x1000)

    print("reload field +0x9c, scaled by the 0x5f25d8 constant:")
    print("   stored | displayed | stored/30")
    bad = 0
    for ticks in (1, 15, 30, 45, 90, 300):
        got = run(icd, RELOAD, 0x9c, ticks, 2)
        want = ticks / 30.0
        ok = abs(got - want) < 1e-9
        bad += not ok
        print("   %6d | %9.6f | %9.6f  %s" % (ticks, got, want, "" if ok else "MISMATCH"))
    print("  => a stored 90 displays as 3 seconds: the field is a count of 30Hz ticks.\n")

    print("a 16.16 field (+0x172 here), scaled by the 0x5eba78 constant:")
    print("      stored |  displayed | stored/65536")
    for px in (1.0, 16.0, 100.5, 2048.25):
        raw = int(px * 65536)
        got = run(icd, FIXED, 0x172, raw, 4)
        ok = abs(got - px) < 1e-9
        bad += not ok
        print("  %10d | %10.4f | %10.4f  %s" % (raw, got, px, "" if ok else "MISMATCH"))
    print("  => 65536 per pixel, the same 16.16 our Fixed uses.")
    print("\n%s" % ("all scales confirmed" if not bad else "%d MISMATCHES" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
