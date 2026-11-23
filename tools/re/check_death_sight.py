#!/usr/bin/env python3
"""Check retail's death sight (the revealer list) against KINGDOMS.icd.

Runs retail's 0x4c6750(unit, ticks) (keep a revealer), 0x4c6b30 (expire them,
once per tick) and the gate in the unit-retire routine 0x512ae0, with the
footprint removal 0x4c6800 hooked to record what it is handed.

It asserts the rule World::updateViewCounts builds on (src/sim/sim.cpp):

- with LosOn off ([[0x62d558]+8]+0x15 == 0) 4c6750 does nothing at all;
- the list is one global per machine (count 0x62da20, 32-byte slots at
  0x62d7a0) and holds at most 20; past the cap the footprint is removed at
  once and nothing is evicted;
- a slot is the unit's 32-byte sight object (unit+0x84) with the expiry at
  +4 = tick (game+0x19f44) + 60, kept in death order; unit+0xa3 is cleared;
- 4c6b30 keeps a slot while expiry >= now and removes it (4c6800) once
  expiry < now; the list compacts in order;
- 512ae0 keeps a revealer only when the owner's index (record +0xeb) equals
  game+0x306f, the VIEWED player (not 0x306e, the local one); any other
  owner's footprint is removed at once. So a machine's list only ever holds
  one owner's units, and the 20 is per owner.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_death_sight.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX

GAME, GLOB, SET = HEAP + 0x40000, HEAP + 0x93000, HEAP + 0x93100
UNITS, REC = HEAP + 0x100000, HEAP + 0x90000
COUNT, SLOTS = 0x62da20, 0x62d7a0


def main():
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def put8(a, v): uc.mem_write(a, bytes((v & 0xff,)))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    def byte(a): return uc.mem_read(a, 1)[0]
    uc.mem_write(GAME, bytes(0x1a000))
    put(0x62d55c, GAME); put(0x62d558, GLOB); put(GLOB + 8, SET)
    removed = []
    def rm(uc, sp):
        removed.append(uc.reg_read(UC_X86_REG_ECX)); return 2, 0
    icd.hooks[0x4c6800] = rm
    checks = 0
    def check(cond, label):
        nonlocal checks
        if not cond: sys.exit('FAIL: ' + label)
        checks += 1
    def unit(i):
        u = UNITS + 0x200 * i; uc.mem_write(u, bytes(0x200))
        uc.mem_write(u + 0x84, bytes([i]) * 32); put8(u + 0xa3, 1); return u

    # LosOn off: nothing kept, nothing removed.
    put8(SET + 0x15, 0); put(COUNT, 0); put(GAME + 0x19f44, 1000)
    u = unit(1); icd.call(0x4c6750, (u, 60), ecx=u + 0x84)
    check(get(COUNT) == 0 and not removed and byte(u + 0xa3) == 1,
          'LosOn off: no revealer, footprint left alone')

    # LosOn on: 20 kept, the rest removed at once.
    put8(SET + 0x15, 1)
    for i in range(25):
        put(GAME + 0x19f44, 1000 + i); u = unit(i); icd.call(0x4c6750, (u, 60), ecx=u + 0x84)
    check(get(COUNT) == 20, 'cap: the count stops at 20, got %d' % get(COUNT))
    check(len(removed) == 5 and removed[0] == UNITS + 0x200 * 20 + 0x84,
          'past the cap: the footprint is removed at once, nothing evicted')
    check(byte(SLOTS) == 0 and get(SLOTS + 4) == 1000 + 60,
          'a slot is the 32-byte copy of unit+0x84 with expiry tick+60 at +4')
    check(byte(SLOTS + 0x20 * 19) == 19 and get(SLOTS + 0x20 * 19 + 4) == 1019 + 60,
          'slot 19 holds the 20th death, in order')
    check(byte(UNITS + 0xa3) == 0, 'unit+0xa3 cleared on keep')

    # 4c6b30: expiry == now stays, expiry < now goes; the list compacts.
    removed.clear(); put(GAME + 0x19f44, 1060); icd.call(0x4c6b30)
    check(get(COUNT) == 20 and not removed, 'expiry == now: kept')
    put(GAME + 0x19f44, 1061); icd.call(0x4c6b30)
    check(get(COUNT) == 19 and removed == [SLOTS], 'expiry < now: removed, the list compacts')
    check(byte(SLOTS) == 1, 'compaction keeps order')

    # 512ae0: the owner's index against game+0x306f (viewed player).
    stop = [None]
    def at_end(uc, a, size, _): stop[0] = a; uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, at_end, begin=0x512b34, end=0x512b34)
    kept = []
    def keep(uc, sp): kept.append(struct.unpack('<I', uc.mem_read(sp + 4, 4))[0]); return 2, 0
    icd.hooks[0x4c6750] = keep
    for owner, view, local in ((2, 2, 2), (3, 2, 2), (3, 3, 2), (2, 3, 2)):
        kept.clear(); removed.clear()
        u = unit(30); put(u + 0x130, 0x1000000); put(u + 0xb8, REC); put8(REC + 0xeb, owner)
        put8(GAME + 0x306f, view); put8(GAME + 0x306e, local)
        icd.call(0x512ae0, (u,), allow_early_stop=True)
        k = bool(kept)
        check(stop[0] == 0x512b34 and k == (owner == view) and (not k) == bool(removed) and
              (kept == [60] if k else True),
              f'512ae0 owner={owner} view(306f)={view} local(306e)={local}')
    print(f'PASS: {checks} retail death-sight checks: LosOn-gated, 20 per machine (= per owner), '
          'no eviction, expiry kept while >= now, kept only for the viewed player\'s units')


if __name__ == '__main__': main()
