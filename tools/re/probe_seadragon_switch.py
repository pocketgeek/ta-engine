#!/usr/bin/env python3
"""Observe why a computer-owned Sea Dragon on Hold Position never fires its
Fire Breath at an enemy 472 px off (inside Fire Breath's 525 and Water Ball's
800, outside the Tsunami's 450).

Runs the fire-at-will sweep 0x40f330 natively, call after call, on one
computer-owned switcher with the Sea Dragon's three slots (VERDRAG.FBI:
Fire Breath aoe 40, 795 damage; Water Ball aoe 50, 300 mana; Tsunami aoe 180,
900 mana; 1000 mana), the weapon switch 0x51a8b0 and its TargetCleared step
0x51a7f0 native too, and the game RNG 0x535cc0 native on a fixed seed.
Observed:

  1. With Fire Breath selected and holding the enemy (30000 HP, alone, no
     ally near it) the switch block takes the Tsunami (slot 2) before the
     Water Ball: it never looks at a slot's range (no call of the range
     0x52aba0 or the in-range tests from the block).
  2. 0x51a8b0 gives the new slot its switchreloadtime (weapon +0x9e, which
     the FBI parser 0x5307ff..0x53082b defaults to reloadtime*30): selecting
     Fire Breath again restarts its 90-tick reload.
  3. The loop: the Tsunami's own sweep finds nothing in its 450 (the slot
     sweep 0x40f2e0 is answered here by range alone: the enemy is in reach of
     a slot whose range is at least 472); with no target the costly Tsunami
     goes back to the primary on rand(10) == 0; Fire Breath takes the enemy
     in that same visit and gives it up for the Tsunami on the next. Fire
     Breath is never selected for its 90 ticks, so it never fires. With 600
     mana (the Tsunami unaffordable) the block takes the Water Ball, which
     reaches, instead.

Hooked instead of run: the slot target 0x51a9a0 (answered from the
probe's own slot state), the slot sweep 0x40f2e0, the list queries 0x411ae0
(enemies near the target: the target itself) and 0x411dd0 (allies near it:
none), the script start 0x56c640, the range 0x52aba0 (logged, must not be
called) and 0x4eba00 (delete).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/probe_seadragon_switch.py
"""
import struct

from emu import Icd, HEAP

UNITS, USTRIDE = HEAP + 0x80000, 0x138
DEFS, DSTRIDE = HEAP + 0xa0000, 0x300
RECS = HEAP + 0x101000
WEAPONS = HEAP + 0x103000
CLASSES = HEAP + 0x104000
STUBS = HEAP + 0x105000
SWEEP = HEAP + 0x106000
GLOB_P, GLOB_Q, SETTINGS = HEAP + 0x107000, HEAP + 0x107100, HEAP + 0x107200
MASKS = HEAP + 0x107400
LIST = HEAP + 0x108000
STUB_FALSE = STUBS
GAME = HEAP + 0x40000

p = Icd()
uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
putf = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
get16 = lambda a: struct.unpack('<H', uc.mem_read(a, 2))[0]
uc.mem_write(STUB_FALSE, bytes((0x31, 0xc0, 0xc3)))
put(0x62d55c, GAME)
put(0x62d558, GLOB_P); put(GLOB_P, GLOB_Q); put(GLOB_P + 8, SETTINGS); put8(GLOB_Q + 7, 0)
put(SETTINGS + 0xc, 30)
uc.mem_write(MASKS, bytes(0x40))

DRAGON, ENEMY, EMPTY = UNITS + 40 * USTRIDE, UNITS + 41 * USTRIDE, UNITS + 39 * USTRIDE
# (name, aoe, damage, manapershot, range, switchreloadtime in ticks)
SLOTS = [('Fire Breath', 40, 795, 0.0, 525, 90),
         ('Water Ball', 50, 795, 300.0, 800, 75),
         ('Tsunami', 180, 3150, 900.0, 450, 150)]
DISTANCE = 472

state = {'target': [0, 0, 0]}
log = {}


def selected():
    return get(DRAGON + 0x130) >> 30


def slot_target(uc_, sp):        # 51a9a0(unit, slot)
    unit, slot = struct.unpack('<2I', uc.mem_read(sp, 8))
    return 2, state['target'][slot & 0xff] if unit == DRAGON else 0


def sweep_slot(uc_, sp):         # 40f2e0(unit, slot): the slot's own sweep, by range
    unit, slot = struct.unpack('<2I', uc.mem_read(sp, 8))
    slot &= 0xff
    log.setdefault('swept', []).append(slot)
    state['target'][slot] = ENEMY if SLOTS[slot][4] >= DISTANCE else 0
    return 2, 0


def nearby(found):
    def f(uc_, sp):              # 411ae0 / 411dd0(player, pos, radius, flag, out)
        _, _, radius, _, out = struct.unpack('<5I', uc.mem_read(sp, 20))
        ptrs = found()
        for i, a in enumerate(ptrs):
            put(LIST + 4 * i, a)
        put(out + 4, LIST); put(out + 8, LIST + 4 * len(ptrs)); put(out + 12, LIST + 4 * len(ptrs))
        return 5, 0
    return f


def target_cleared(uc_, sp):     # 51a7f0 runs natively; its script start is logged
    return 8, 0


def script_start(uc_, sp):       # 56c640(name, ...), ecx = the script
    name = get(sp)
    text = bytes(uc.mem_read(name, 16)).split(b'\0')[0].decode()
    log.setdefault('scripts', []).append(text)
    return 8, 0


p.hooks.update({
    0x51a9a0: slot_target,
    0x40f2e0: sweep_slot,
    0x411ae0: nearby(lambda: [ENEMY]),
    0x411dd0: nearby(lambda: []),
    0x56c640: script_start,
    0x52aba0: lambda uc_, sp: (log.setdefault('range', []).append(1), (2, 0))[1],
    0x4eba00: lambda uc_, sp: (0, 0),
})
p.freeze_hooks()
checks = 0


def check(ok, label):
    global checks
    if not ok:
        raise AssertionError(label)
    checks += 1
    print('ok  ', label)


def setup(mana, sel):
    for u in (DRAGON, ENEMY, EMPTY):
        uc.mem_write(u, bytes(USTRIDE))
    d, td = DEFS + 40 * DSTRIDE, DEFS + 41 * DSTRIDE
    uc.mem_write(d, bytes(DSTRIDE)); uc.mem_write(td, bytes(DSTRIDE))
    put(DRAGON + 0xb4, d); put(ENEMY + 0xb4, td); put(EMPTY + 0xb4, d)
    armed = 0x1000000 | 0x8000000 | 0x80000 | 1          # live, fire at will, Hold Position
    put(DRAGON + 0x130, armed | (sel << 30)); put(DRAGON + 0xb8, RECS + 0x200)
    putf(DRAGON + 0xd8, mana)
    put(RECS + 0x200, 1); put8(RECS + 0x200 + 0xea, 2)       # computer-owned
    put(d + 0x260, 0x10000); put(d + 0x268, 0x10)            # weaponswitching; noveteran
    for s, (_, aoe, damage, cost, _, reload) in enumerate(SLOTS):
        w, cls = WEAPONS + 0x100 * s, CLASSES + 0x40 * s
        uc.mem_write(w, bytes(0x100))
        put(w + 0x40, cls); put(cls, cls + 0x10)
        for off in (0x14, 0x1c, 0x20, 0x24):
            put(cls + 0x10 + off, STUB_FALSE)
        put16(w + 0x88, damage); put16(w + 0x8a, aoe); putf(w + 0xd4, cost); put16(w + 0x9e, reload)
        put(DRAGON + 0xc + 0x1c * s, w); put(d + 0x1aa + 4 * s, w); put(d + 0x8e + 4 * s, MASKS)
        put8(DRAGON + 0xc + 0x1c * s + 0x1a, 4)              # automatic
    put(ENEMY + 0x130, 0x1000000 | 1); put16(ENEMY + 0x10c, 30000); put(ENEMY + 0xb8, RECS + 0x100)
    rec = RECS + 0x200
    put(rec + 0x74, EMPTY); put(rec + 0x78, DRAGON)
    uc.mem_write(SWEEP, bytes(0x200)); put(SWEEP, rec); put(SWEEP + 0x1a1, EMPTY)
    state['target'] = [0, 0, 0]


def visit():
    put(SWEEP + 0x1a1, EMPTY)    # the walk visits every slot but the one it starts on
    _, error = p.call(0x40f330, (1,), ecx=SWEEP)
    assert error is None, error


def reload_of(slot):
    return get16(DRAGON + 0xc + 0x1c * slot + 0x14)


# ---- 1 and 2: one visit with Fire Breath holding the enemy --------------------
put(0x64186c, 0x12345679)
setup(1000.0, 0)
state['target'][0] = ENEMY
log.clear()
visit()
check(selected() == 2, f'Fire Breath holding the enemy: the block selects the Tsunami (slot {selected()})')
check(not log.get('range'), 'the switch block calls no range test')
check(reload_of(2) == 150 and 'SwitchWeapon' in log.get('scripts', []),
      f'51a8b0 gives the Tsunami its switchreloadtime ({reload_of(2)} ticks) and starts SwitchWeapon')
check(log.get('swept') == [2] and state['target'][2] == 0,
      'the Tsunami is swept in the same visit and finds nothing in its 450')

# ---- 3: the loop over many visits ---------------------------------------------
put(0x64186c, 0x12345679)
setup(1000.0, 0)
run, longest, fire_breath_visits, back = 0, 0, 0, 0
reloads = []
for _ in range(3000):
    before = selected()
    visit()
    now = selected()
    if now == 0:
        fire_breath_visits += 1
        run += 1
        longest = max(longest, run)
        if before != 0:
            back += 1
            reloads.append(reload_of(0))
    else:
        run = 0
print(f'     3000 visits: Fire Breath selected after {fire_breath_visits}, {back} times back to it, '
      f'longest run {longest} visit(s)')
check(back > 100 and longest == 1,
      'the Tsunami goes back to Fire Breath on rand(10) == 0 and leaves it on the next visit')
check(reloads and all(r == 90 for r in reloads), 'each return restarts the Fire Breath reload at 90 ticks')

# ---- the same with the Tsunami unaffordable -----------------------------------
setup(600.0, 0)
state['target'][0] = ENEMY
log.clear()
visit()
check(selected() == 1 and state['target'][1] == ENEMY,
      f'600 mana: the block takes the Water Ball (slot {selected()}), which reaches')
print(f'PASS: {checks} observations')
