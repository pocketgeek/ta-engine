#!/usr/bin/env python3
"""Retail Guard_NoMove's armed step 0x401a90(unit), emulated (2026-11-03).

Runs the real 0x401a90 and the real slot setters 0x519b70 / 0x519b10
(KINGDOMS.icd).  Hooked: the slot target getter 0x51a9a0, the bad-target
test 0x52adf0 and the in-range test 0x52ad60 (scripted per slot), the slot
target clear 0x51a7f0 (logged), and rand 0x535cc0 (scripted, logged).
Reads: which slots end automatic (slot flags word +0x1a bit 4), which get
their target cleared, how many draws are taken, and what happens to the
per-slot bits +0xd0 0x200000 << slot.  Observation only; reads the user's
own retail binary; nothing is copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
get16 = lambda a: struct.unpack('<H', uc.mem_read(a, 2))[0]
U, D = HEAP + 0x1000, HEAP + 0x2000
SLOT = lambda i: U + 0xc + 0x1c * i

state = {}
log = []
def target(uc_, sp):
    unit, slot = struct.unpack('<2I', uc.mem_read(sp, 8))
    return 2, state['targets'][slot & 0xff]
def bad(uc_, sp):
    unit, t, slot = struct.unpack('<3I', uc.mem_read(sp, 12))
    return 3, int(state['bad'][slot & 0xff])
def inrange(uc_, sp):
    unit, t, slot = struct.unpack('<3I', uc.mem_read(sp, 12))
    return 3, int(state['inrange'][slot & 0xff])
def clear(uc_, sp):
    unit, slot = struct.unpack('<2I', uc.mem_read(sp, 8))
    log.append(('clear', slot & 0xff)); return 2, 0
def rand(uc_, sp):
    n = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    v = state['draws'].pop(0); log.append(('rand', n, v)); return 1, v
p.hooks.update({0x51a9a0: target, 0x52adf0: bad, 0x52ad60: inrange,
                0x51a7f0: clear, 0x535cc0: rand})
p.freeze_hooks()

checks = 0
def check(ok, label):
    global checks
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: raise SystemExit(1)
    checks += 1

def run(present=(1, 1, 1), auto=(1, 1, 1), bits=0, targets=(0, 0, 0), badt=(0, 0, 0),
        inr=(1, 1, 1), draws=(), switcher=False, selected=0):
    uc.mem_write(U, bytes(0x138)); uc.mem_write(D, bytes(0x300)); put(U + 0xb4, D)
    put(U + 0x130, 0x1000000 | 0x8000000 | (selected << 30))
    if switcher: put(D + 0x260, 0x10000)
    for i in range(3):
        put(SLOT(i), 0x5000 + i if present[i] else 0)
        put16(SLOT(i) + 0x1a, 4 if auto[i] else 0)
    put(U + 0xd0, bits | 0x1)
    state.update(targets=list(targets), bad=list(badt), inrange=list(inr), draws=list(draws))
    log.clear()
    r, err = p.call(0x401a90, (U,)); assert err is None, err
    flags = [bool(get16(SLOT(i) + 0x1a) & 4) for i in range(3)]
    return flags, list(log), get(U + 0xd0)

# 1. 519b70 then 519b10 on an automatic slot with a live target in reach:
#    nothing happens to it (the loop skips a good slot).
f, l, d = run(auto=(1, 0, 1), targets=(7, 7, 7))
check(f == [True, False, True] and l == [], f'all targets good and in reach: slots unchanged, no clear, no draw ({f}, {l})')
# 2. Out of reach: the pair runs. A manual slot becomes automatic, its target
#    cleared once (by 519b10); an automatic slot is cleared twice (519b70 makes
#    it manual and clears, 519b10 makes it automatic and clears).
f, l, d = run(auto=(0, 1, 1), targets=(7, 7, 7), inr=(0, 0, 1))
check(f == [True, True, True], f'out of reach: slots 0 (manual) and 1 (automatic) end automatic: {f}')
check(l == [('clear', 0), ('clear', 1), ('clear', 1)],
      f'519b70+519b10: manual slot cleared once, automatic slot cleared twice, slot 2 (in reach) untouched: {l}')
# 3. No target and bad target also release the slot.
f, l, d = run(auto=(0, 0, 1), targets=(0, 7, 7), badt=(0, 1, 0))
check(f == [True, True, True] and ('clear', 0) in l and ('clear', 1) in l and all(x[1] != 2 for x in l),
      f'no target (slot 0) or bad target (slot 1): released; good slot 2 kept: {f} {l}')
# 4. Absent slots are skipped.
f, l, d = run(present=(1, 0, 0), auto=(0, 0, 0), targets=(0, 0, 0))
check(f == [True, False, False] and l == [('clear', 0)], f'absent slots skipped: {f} {l}')
# 5. A switcher (def+0x260 & 0x10000) only treats its selected slot (+0x130 >> 30).
f, l, d = run(auto=(0, 0, 0), targets=(0, 0, 0), switcher=True, selected=1)
check(f == [False, True, False] and l == [('clear', 1)], f'switcher: only the selected slot 1: {f} {l}')
# 6. The +0xd0 slot bits: no draw unless one is set.
f, l, d = run(targets=(7, 7, 7))
check(not any(x[0] == 'rand' for x in l), 'no +0xd0 slot bit: no draw')
# 7. A bit set, rand(3) != 0: no release, but every bit is cleared.
f, l, d = run(auto=(0, 0, 0), bits=0x600000, targets=(7, 7, 7), draws=[1])
check(l == [('rand', 3, 1)] and f == [False, False, False] and d & 0xe00000 == 0,
      f'bits 0x600000, rand(3)=1: one draw, no release, bits cleared: {f} {l} d={d:#x}')
# 8. rand(3) == 0: the flagged slots are released (pair), then the bits cleared.
f, l, d = run(auto=(0, 0, 0), bits=0x600000, targets=(7, 7, 7), draws=[0])
check(f == [True, True, False] and [x for x in l if x[0] == 'clear'] == [('clear', 0), ('clear', 1)]
      and d & 0xe00000 == 0 and d & 1,
      f'bits 0x600000 (slots 0, 1), rand(3)=0: slots 0 and 1 released, bits cleared, other bits kept: {f} {l} d={d:#x}')
# 9. Order: the bit release precedes the reach loop; a released slot whose
#    target is still in reach is not touched again.
f, l, d = run(auto=(1, 1, 1), bits=0x200000, targets=(7, 7, 7), draws=[0])
check(l == [('rand', 3, 0), ('clear', 0), ('clear', 0)], f'released slot 0 automatic: cleared twice, loop adds nothing: {l}')
# 10. Who sets the bits: 0x4099e0(shot, damage to an enemy, damage to an own
#     unit), called by the impact dispatch (0x52a316 area, 0x52a4c4 direct hit).
#     Enemy damage > 2 x own damage: +0xd0 |= 0x1000000; otherwise the shot's
#     slot (+0xd8 bits 2-3) gets 0x200000 << slot.
SHOT = HEAP + 0x3000
for enemy_dmg, own_dmg, slotbits, want in ((50, 10, 0, 0x1000000), (20, 10, 0, 0x200000),
                                            (0, 0, 4, 0x400000), (30, 20, 8, 0x800000), (0, 5, 0xc, 0x800000)):
    uc.mem_write(SHOT, bytes(0x100)); put(SHOT + 0x7c, U); put(SHOT + 0xd8, slotbits); put(U + 0xd0, 0)
    r, err = p.call(0x4099e0, (SHOT, enemy_dmg, own_dmg), ecx=0); assert err is None, err
    check(get(U + 0xd0) == want, f'4099e0 enemy {enemy_dmg}, own {own_dmg}, shot slot bits {slotbits:#x}: +0xd0 = {get(U + 0xd0):#x}')
print(f'PASS: {checks} observations')
