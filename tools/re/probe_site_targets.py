#!/usr/bin/env python3
"""Observe how retail's target scans treat enemies with construction left.

A unit with construction left (+0x108, a float: 1.0 for a fresh site, 0 once
finished) is a conjured site still being built, or a unit still being built at
a factory door. Four parts, each running the routine natively on controlled
units and checking what World ports:

  1. 0x411700, the player's list rebuild: an enemy with construction left goes
     on the visible-enemy list +5 like a finished one. The list drops only a
     live enemy's scenario flag 0x2000 and what the player does not see
     (0x4f7210, answered per unit); allies (own sites included) go on +0x15.
  2. 0x4d8370, the mission scan (Standby, Patrol, a move's response, the VTOL
     missions): Fire At Will and armed only; it calls the picker 0x412620 on
     the scanning slot (0x519ac0: a switcher's selected slot, else the first
     weapon) with the mission radius, the larger of sightdistance and the
     slot's range. A lone site in reach is returned (the picker's x10 for
     construction left only ranks; tools/re/check_fire_sweep_picker.py
     compares the whole picker with the port).
  3. 0x40f330, the fire-at-will sweep's budget: N/30 + 1 counted units per
     call. A dead or dying unit, or one with construction left, costs nothing
     (0x40f3a1..0x40f3c9); a paralyzed unit costs one like any other.
  4. 0x40f330's AI weapon switch block: an AI switcher whose selected slot
     holds a site takes no stone slot against it (0x40f7df..0x40f7f0); the
     same target finished is switched to.

Logged instead of run: the visibility test, the lists' erase and insert, the
list query 0x411ae0, the range 0x52aba0, the in-range and minimum-range tests,
the damage, the slot target 0x51a9a0, the slot sweep 0x40f2e0 and the switch
0x51a8b0.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/probe_site_targets.py
"""
import struct

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

GAME = HEAP + 0x40000
UNITS, USTRIDE = HEAP + 0x80000, 0x138
DEFS, DSTRIDE = HEAP + 0xa0000, 0x300
CACHE = HEAP + 0x100000
RECS = HEAP + 0x101000          # player records, 0x100 apart
TYPETABLE = HEAP + 0x102000
WEAPONS = HEAP + 0x103000       # 0x100 apart
CLASSES = HEAP + 0x104000       # class objects and their vtables, 0x40 apart
STUBS = HEAP + 0x105000
SWEEP = HEAP + 0x106000
GLOB_P, GLOB_Q, SETTINGS = HEAP + 0x107000, HEAP + 0x107100, HEAP + 0x107200
MASKS = HEAP + 0x107400
LIST = HEAP + 0x108000
STUB_FALSE, STUB_TRUE = STUBS, STUBS + 0x10

p = Icd()
uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
putf = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
uc.mem_write(STUB_FALSE, bytes((0x31, 0xc0, 0xc3)))
uc.mem_write(STUB_TRUE, bytes((0x31, 0xc0, 0xb0, 0x01, 0xc3)))
put(0x62d55c, GAME)
put(0x62d558, GLOB_P); put(GLOB_P, GLOB_Q); put(GLOB_P + 8, SETTINGS); put8(GLOB_Q + 7, 0)
for i in range(4):
    put8(RECS + 0x100 * i + 0xeb, i)
    put(RECS + 0x100 * i, 1)
# Player 0's alliance table: itself and player 2 allied, players 1 and 3 not.
for other, allied in ((0, 1), (1, 0), (2, 1), (3, 0)):
    put8(RECS + 0xac + other, allied)

log = {}
visible = {}
pushes = []


def clear_unit(i):
    u, d = UNITS + i * USTRIDE, DEFS + i * DSTRIDE
    uc.mem_write(u, bytes(USTRIDE)); uc.mem_write(d, bytes(DSTRIDE))
    put(u + 0xb4, d)
    return u, d


def weapon_class(index, melee=False, stone=False):
    w, cls = WEAPONS + 0x100 * index, CLASSES + 0x40 * index
    uc.mem_write(w, bytes(0x100))
    put(w + 0x40, cls); put(cls, cls + 0x10)
    for slot, on in ((0x14, melee), (0x1c, stone), (0x20, False), (0x24, False)):
        put(cls + 0x10 + slot, STUB_TRUE if on else STUB_FALSE)
    return w


def hook(name):
    def f(uc_, sp):
        n, ret = SPECS[name]
        log.setdefault(name, []).append(struct.unpack(f'<{n}I', uc.mem_read(sp, 4 * n)) if n else ())
        return n, ret() if callable(ret) else ret
    return f


def insert(uc_, sp):            # 409510(pos, 1, &value), ecx = the list
    lst = uc.reg_read(UC_X86_REG_ECX)
    _, _, value = struct.unpack('<3I', uc.mem_read(sp, 12))
    pushes.append((lst - CACHE, get(value)))
    return 3, 0


def query(uc_, sp):             # 411ae0(player, pos, radius, flag, out)
    player, pos, radius, flag, out = struct.unpack('<5I', uc.mem_read(sp, 20))
    log.setdefault('query', []).append((player, radius, flag))
    ptrs = log.get('candidates', [])
    for i, a in enumerate(ptrs):
        put(LIST + 4 * i, a)
    put(out + 4, LIST); put(out + 8, LIST + 4 * len(ptrs)); put(out + 12, LIST + 4 * len(ptrs))
    return 5, 0


SPECS = {
    'range': (2, lambda: log['range_value']),
    'slot_target': (2, lambda: log.get('slot_target_value', 0)),
    'sweep_slot': (2, 0),
    'switch': (2, 0),
}
p.hooks.update({
    0x4f7210: lambda uc_, sp: (2, visible.get(struct.unpack('<2I', uc.mem_read(sp, 8))[1], 0)),
    0x445a40: lambda uc_, sp: (2, 0),
    0x409510: insert,
    0x411ae0: query,
    0x52aba0: hook('range'),
    0x52ad60: lambda uc_, sp: (3, 1),
    0x52adf0: lambda uc_, sp: (3, 0),
    0x531e50: lambda uc_, sp: (1, 100),
    0x4eba00: lambda uc_, sp: (0, 0),
    0x51a9a0: hook('slot_target'),
    0x40f2e0: hook('sweep_slot'),
    0x51a8b0: hook('switch'),
})
p.freeze_hooks()
checks = 0


def check(ok, label):
    global checks
    if not ok:
        raise AssertionError(label)
    checks += 1
    print('ok  ', label)


# ---- 1. 0x411700 -------------------------------------------------------------
# (player, flags, construction left, seen)
rows = [
    ('own finished', 0, 0x1000000, 0.0, 1),
    ('own site', 0, 0x1000000, 1.0, 1),
    ('enemy finished', 1, 0x1000000, 0.0, 1),
    ('enemy site, fresh', 1, 0x1000000, 1.0, 1),
    ('enemy site, a quarter left', 1, 0x1000000, 0.25, 1),
    ('enemy finished, unseen', 1, 0x1000000, 0.0, 0),
    ('enemy site, unseen', 1, 0x1000000, 1.0, 0),
    ('enemy, scenario flag 0x2000', 1, 0x1002000, 0.0, 1),
    ('enemy site, dying', 1, 0x1001000, 1.0, 1),
    ('ally site', 2, 0x1000000, 0.5, 1),
    ('enemy site of a third player', 3, 0x1000000, 1.0, 1),
]
uc.mem_write(CACHE, bytes(0x200))
put(CACHE, RECS)
put(CACHE + 0xb5, TYPETABLE)
units = []
for i, (name, player, flags, left, seen) in enumerate(rows, 1):
    u, d = clear_unit(i)
    put(u + 0x130, flags); putf(u + 0x108, left)
    put(u + 0xb8, RECS + 0x100 * player)
    visible[u] = seen
    units.append(u)
put(GAME + 0x14e84, UNITS)                      # the walk starts one record in
put(GAME + 0x14e88, UNITS + len(rows) * USTRIDE)
_, error = p.call(0x411700, (), ecx=CACHE)
assert error is None, error
enemies = [rows[units.index(v)][0] for lst, v in pushes if lst == 5]
allies = [rows[units.index(v)][0] for lst, v in pushes if lst == 0x15]
check(enemies == ['enemy finished', 'enemy site, fresh', 'enemy site, a quarter left', 'enemy site of a third player'],
      f'411700 list +5 holds every seen live enemy, construction left or not: {enemies}')
check(allies == ['own finished', 'own site', 'ally site'],
      f'411700 list +0x15 holds the seen allies, own sites included: {allies}')

# ---- 2. 0x4d8370 -------------------------------------------------------------
def shooter(flags, sight, switcher=False, selected=0, weapons=(True, False, False)):
    u, d = clear_unit(20)
    put(u + 0x130, flags | (selected << 30)); put(u + 0xb8, RECS); put8(u + 0xfd, 0)
    put(u + 0x68, 1024 << 16); put(u + 0x70, 1024 << 16)
    put(d + 0x260, 0x10000 if switcher else 0); put16(d + 0x226, sight)
    put(d + 0x9a, MASKS); put(d + 0x8e, MASKS); put(d + 0x92, MASKS); put(d + 0x96, MASKS)
    for slot, present in enumerate(weapons):
        if present:
            w = weapon_class(slot)
            put(u + 0xc + 0x1c * slot, w); put(d + 0x1aa + 4 * slot, w)
    return u


def target(i, left, x=1124):
    u, d = clear_unit(i)
    put(u + 0x130, 0x1000000 | 1); putf(u + 0x108, left); put16(u, i)
    put(u + 0x68, x << 16); put(u + 0x70, 1024 << 16); put(d + 0x260, 0x8000)
    return u


uc.mem_write(MASKS, bytes(0x40))
put(0x64186c, 0x12345679)
site = target(21, 1.0)
log.clear(); log['candidates'] = [site]; log['range_value'] = 300
for flags, name in ((0x8000000, 'Hold Fire'), (0x8000000 | 0x40000, 'Return Fire'), (0x80000, 'unarmed')):
    log['query'] = []
    result, _ = p.call(0x4d8370, (shooter(0x1000000 | flags, 400),))
    check(result == 0 and not log['query'], f'4d8370 does not scan on {name}')
for sight, want in ((400, 400), (250, 300)):
    log['query'] = []; log['range'] = []
    result, _ = p.call(0x4d8370, (shooter(0x1000000 | 0x8000000 | 0x80000, sight),))
    check(result == site and log['query'] == [(0, want, 0)],
          f'4d8370 returns a lone site; mission radius max(sight {sight}, range 300) = {want}')
log['range'] = []
p.call(0x4d8370, (shooter(0x1000000 | 0x8000000 | 0x80000, 400, True, 1, (True, True, False)),))
check(log['range'] and all(args[1] & 0xff == 1 for args in log['range']), '4d8370 scans with a switcher\'s selected slot')
log['range'] = []
p.call(0x4d8370, (shooter(0x1000000 | 0x8000000 | 0x80000, 400, False, 0, (False, True, False)),))
# 519ac0 returns the slot in al only (412620 masks it); the rest of eax is
# whatever the slot test left there.
check(log['range'] and all(args[1] & 0xff == 1 for args in log['range']), '4d8370 scans with the first weapon a unit has')

# ---- 3. 0x40f330, the budget -------------------------------------------------
def sweep_units(specs):
    """specs: (name, flags, construction left, paralyzed) in slot order."""
    ptrs = []
    for i, (name, flags, left, paralyzed) in enumerate(specs, 30):
        u, d = clear_unit(i)
        put(u + 0x130, flags); putf(u + 0x108, left); put8(u + 0x114, 0x10 if paralyzed else 0)
        put(u + 0xb8, RECS + 0x100)                   # a human owner
        w = weapon_class(10)
        put(u + 0xc, w); put8(u + 0x26, 4)            # slot 0 automatic
        ptrs.append(u)
    rec = RECS + 0x100
    put(rec + 0x74, ptrs[0]); put(rec + 0x78, ptrs[-1])
    uc.mem_write(SWEEP, bytes(0x200)); put(SWEEP, rec); put(SWEEP + 0x1a1, ptrs[-1])
    return ptrs


armed = 0x1000000 | 0x8000000 | 0x80000 | 1
specs = [('a', armed, 0.0, 0), ('own site', armed, 1.0, 0), ('own site, half', armed, 0.5, 0),
         ('b', armed, 0.0, 0), ('paralyzed', armed, 0.0, 1), ('c', armed, 0.0, 0)]
ptrs = sweep_units(specs)
put(SETTINGS + 0xc, 30)                               # N = 30: budget 2
names = lambda calls: [specs[ptrs.index(c[0])][0] for c in calls]
log.clear(); log['sweep_slot'] = []
p.call(0x40f330, (0,), ecx=SWEEP)
first = (names(log['sweep_slot']), specs[ptrs.index(get(SWEEP + 0x1a1))][0])
log['sweep_slot'] = []
p.call(0x40f330, (0,), ecx=SWEEP)
second = (names(log['sweep_slot']), specs[ptrs.index(get(SWEEP + 0x1a1))][0])
check(first == (['a', 'b'], 'b'), f'40f330 passes its own sites at no cost (budget 2): swept, cursor {first}')
check(second == (['paralyzed', 'c'], 'c'), f'40f330 counts a paralyzed unit like any other: swept, cursor {second}')

# ---- 4. 0x40f330, the AI weapon switch ---------------------------------------
def switch_case(left):
    u, d = clear_unit(40)
    put(u + 0x130, armed); put(u + 0xb8, RECS + 0x200)
    put(RECS + 0x200, 1); put8(RECS + 0x200 + 0xea, 2)      # computer-owned
    put(d + 0x260, 0x10000); put(d + 0x268, 0x10)            # weaponswitching; noveteran (getMult 1.0)
    for slot in range(3):
        put(d + 0x8e + 4 * slot, MASKS)
    w0 = weapon_class(11); put16(w0 + 0x88, 10)              # the primary: 10 damage
    w1 = weapon_class(12, stone=True)                        # slot 1: a stone class, aoe 0, no mana
    put(u + 0xc, w0); put8(u + 0x26, 4); put(u + 0x28, w1)
    t, td = clear_unit(41)
    put(t + 0x130, 0x1000000 | 1); putf(t + 0x108, left); put16(t + 0x10c, 3000)
    put(t + 0xb8, RECS + 0x100)                              # player 1: not allied to player 2
    # The walk never visits the slot it starts on: start on an empty slot
    # before the switcher.
    empty, _ = clear_unit(39)
    rec = RECS + 0x200
    put(rec + 0x74, empty); put(rec + 0x78, u)
    uc.mem_write(SWEEP, bytes(0x200)); put(SWEEP, rec); put(SWEEP + 0x1a1, empty)
    log.clear(); log['slot_target_value'] = t; log['switch'] = []
    _, error = p.call(0x40f330, (1,), ecx=SWEEP)
    assert error is None, error
    return [args[1] for args in log['switch']]


check(switch_case(0.0) == [1], 'AI switch: a stone slot is taken against a finished target')
check(switch_case(1.0) == [], 'AI switch: no stone slot against a site (construction left)')
check(switch_case(0.25) == [], 'AI switch: ... nor against one a quarter left')
print(f'PASS: {checks} observations')
