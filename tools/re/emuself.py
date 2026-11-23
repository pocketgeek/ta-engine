#!/usr/bin/env python3
"""Run the icd's SelfDestruct mission handler (0x4017e0) and watch what it does.

Builds a fake mission object and a fake unit + unit type, hooks the calls that
would need the live game, and steps the handler the way the mission scheduler
would. Prints the reschedule interval each run and the damage call at the end,
so the countdown and the death are observed rather than inferred.

    pip install unicorn && python3 tools/re/emuself.py
"""
import struct
import sys

from emu import Icd, HEAP
from unicorn.x86_const import *

MISSION = HEAP + 0x1000
UNIT = HEAP + 0x4000
UTYPE = HEAP + 0x8000
GS = HEAP + 0x40000


def run(countdown_bits):
    icd = Icd()
    uc = icd.uc

    log = []

    def sched(uc, argp):                      # 0x4d6a10(this=ecx, delay)
        delay = struct.unpack("<i", uc.mem_read(argp, 4))[0]
        log.append(("reschedule", delay))
        # mirror what it does, so the mission keeps its state machine honest
        this = uc.reg_read(UC_X86_REG_ECX)
        f = struct.unpack("<I", uc.mem_read(this + 6, 4))[0]
        uc.mem_write(this + 6, struct.pack("<I", f | 1))
        return 1, 0

    def damage(uc, argp):                     # 0x51a140(attacker, victim, dmg, type, x)
        a, v, dmg, ty, x = struct.unpack("<iiiii", uc.mem_read(argp, 20))
        log.append(("damage", dmg, ty))
        return 5, 0

    def rnd(uc, argp):                        # 0x535cc0(n) -> pseudo-random
        n = struct.unpack("<i", uc.mem_read(argp, 4))[0]
        return 1, 0                           # deterministic for the trace

    icd.hooks[0x4d6a10] = sched
    icd.hooks[0x51a140] = damage
    icd.hooks[0x535cc0] = rnd

    uc.mem_write(MISSION, b"\0" * 0x400)
    uc.mem_write(UNIT, b"\0" * 0x400)
    uc.mem_write(UTYPE, b"\0" * 0x400)
    uc.mem_write(GS, b"\0" * 0x20000)
    uc.mem_write(0x62d55c, struct.pack("<I", GS))

    # unit -> its type, and the type's selfdestructcountdown in bits 21..23
    uc.mem_write(UNIT + 0xB4, struct.pack("<I", UTYPE))
    uc.mem_write(UTYPE + 0x264, struct.pack("<I", (countdown_bits & 7) << 21))

    print("  selfdestructcountdown = %d" % countdown_bits)
    for step in range(12):
        # the scheduler passes (unit, mission, flags)
        eax, err = icd.call(0x4017e0, args=(UNIT, MISSION, 0), ecx=UNIT)
        if err:
            print("   emulation stopped:", err)
            break
        before = len(log)
        cnt = struct.unpack("<I", uc.mem_read(MISSION + 0x52, 4))[0]
        done = struct.unpack("<I", uc.mem_read(MISSION + 0x4E, 4))[0]
        tail = log[-1] if log else None
        print("   run %2d -> ret=%d  counter=%#010x (%d left)  expired=%d  %s"
              % (step, struct.unpack("<i", struct.pack("<I", eax))[0],
                 cnt, cnt & 0xFFFFFFF, done, tail))
        if any(e[0] == "damage" for e in log):
            break
    print()


if __name__ == "__main__":
    print("SelfDestruct mission (icd 0x4017e0), stepped like the scheduler would.")
    print("bits=2 is the parser DEFAULT when a type omits selfdestructcountdown")
    print("(0x4c0a1f: clear bit 23, set bit 22 -> value 2).\n")
    for bits in (0, 2, 3):
        run(bits)
