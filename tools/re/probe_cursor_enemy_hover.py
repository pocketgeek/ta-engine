#!/usr/bin/env python3
"""Run retail's hover-cursor selector 0x4dd780 over an ENEMY unit.

Default hover (action mode 1) with a `canattack` unit (UnitDef+0x264 bit 0x20)
over an enemy re-enters the selector as ATTACK (mode 3). Mode 3 then splits on
the selected unit's mover pointer (unit+8):

  * a MOBILE unit (mover present) never consults weapon range. It shows Attack
    (slot 1) unless the active weapon cannot hurt the target: damage 0x531e50
    <= 0, the target listed in notargetcategory (UnitDef+0x9a), a stoning /
    freezing weapon class against cantbestoned / cantbefrozen (UnitDef+0x268
    bits 2 / 4), toairweapon (WeaponType+0xc8 0x4000) against a target that is
    not airborne, or noairweapon (0x8000) against an airborne one (unit+0x130
    low bits == 2). Those give TooFar (slot 3).
  * an IMMOBILE unit (no mover) applies the minrange check 0x52adf0 and then
    the damage+inRange check 0x52ad60: Attack only when the target is inside
    the active weapon's range, otherwise TooFar.

The damage lookup, the range helpers and the weapon-class virtuals are
controlled seams (their real bodies are checked by other probes); the
selector's branch structure executes natively. No retail GUI is launched.
"""
import struct

from emu import HEAP, Icd

SELECTOR = 0x4DD780
DAMAGE = 0x531E50       # thiscall WeaponType::damageVs(target)
MIN_RANGE = 0x52ADF0    # stdcall (unit, target, slot): inside minrange
IN_RANGE = 0x52AD60     # stdcall (unit, target, slot): damage > 0 && inRange
ATTACK, TOOFAR = 1, 3

p = Icd()
uc = p.uc
(unit, definition, target, target_def, me, foe, game, settings, options,
 weapon, wclass, vtable, bitset) = [HEAP + i * 0x8000 for i in range(1, 14)]
stub_stone, stub_freeze = HEAP + 0x200000, HEAP + 0x200010


def put(a, v):
    uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def byte(a, v):
    uc.mem_write(a, bytes([v]))


state = {'damage': 1, 'inside_min': 0, 'in_range': 1, 'stones': 0, 'freezes': 0}
p.hooks[DAMAGE] = lambda _uc, _sp: (1, state['damage'])
p.hooks[MIN_RANGE] = lambda _uc, _sp: (3, state['inside_min'])
p.hooks[IN_RANGE] = lambda _uc, _sp: (3, state['in_range'])
p.hooks[stub_stone] = lambda _uc, _sp: (0, state['stones'])
p.hooks[stub_freeze] = lambda _uc, _sp: (0, state['freezes'])
p.freeze_hooks()

put(0x62D55C, game)
put(0x62D558, settings)
put(settings + 0xC, options)
byte(options + 5, 1)
# Two players that are not allied: the selector marks the target as an enemy.
byte(me + 0xE3, 1)
byte(foe + 0xE3, 1)
byte(foe + 0xEB, 1)
put(unit + 0xB4, definition)
put(unit + 0xB8, me)
put(unit + 0xC, weapon)              # WEAPON1; 0x519ac0 picks slot 0
put(definition + 0x264, 0x20)        # canattack
put(definition + 0x9A, bitset)       # notargetcategory bit set, all clear
put(weapon + 0x40, wclass)
put(wclass, vtable)
put(vtable + 0x1C, stub_stone)
put(vtable + 0x20, stub_freeze)
put(target + 0xB4, target_def)
put(target + 0xB8, foe)


def hover(mobile, airborne=False, wflags=0, def268=0, nocat=False, **seams):
    state.update({'damage': 1, 'inside_min': 0, 'in_range': 1, 'stones': 0, 'freezes': 0})
    state.update(seams)
    put(unit + 0x130, 0x01000000)
    put(unit + 8, 1 if mobile else 0)
    put(target + 0x130, 0x01000000 | (2 if airborne else 0))
    put(weapon + 0xC8, wflags)
    put(target_def + 0x268, def268)
    put(bitset, 1 if nocat else 0)   # target unit word 0 is index 0
    got, error = p.call(SELECTOR, (1, unit, target, 0))
    assert error is None, error
    return got


cases = [
    # (label, kwargs, expected slot)
    ('mobile, enemy out of range', dict(mobile=True, in_range=0), ATTACK),
    ('mobile, enemy inside minrange', dict(mobile=True, inside_min=1, in_range=0), ATTACK),
    ('mobile, zero damage', dict(mobile=True, damage=0), TOOFAR),
    ('mobile, notargetcategory', dict(mobile=True, nocat=True), TOOFAR),
    ('mobile, toairweapon vs ground', dict(mobile=True, wflags=0x4000), TOOFAR),
    ('mobile, toairweapon vs airborne', dict(mobile=True, airborne=True, wflags=0x4000), ATTACK),
    ('mobile, noairweapon vs airborne', dict(mobile=True, airborne=True, wflags=0x8000), TOOFAR),
    ('mobile, noairweapon vs ground', dict(mobile=True, wflags=0x8000), ATTACK),
    ('mobile, stoning vs cantbestoned', dict(mobile=True, stones=1, def268=2), TOOFAR),
    ('mobile, stoning vs stoneable', dict(mobile=True, stones=1), ATTACK),
    ('mobile, freezing vs cantbefrozen', dict(mobile=True, freezes=1, def268=4), TOOFAR),
    ('immobile, in range', dict(mobile=False), ATTACK),
    ('immobile, out of range', dict(mobile=False, in_range=0), TOOFAR),
    ('immobile, inside minrange', dict(mobile=False, inside_min=1), TOOFAR),
    ('immobile, notargetcategory', dict(mobile=False, nocat=True), TOOFAR),
]
for label, kwargs, expected in cases:
    got = hover(**kwargs)
    print(f'{label:36s} slot {got}')
    assert got == expected, (label, got, expected)
print('PASS: mobile attackers show Attack over any enemy their weapon can hurt, '
      'regardless of range; immobile ones need the target in range')
