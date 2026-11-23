#!/usr/bin/env python3
"""Run every weapon handler class's unit-target range predicate (vtable +0xc)
natively against a building, to see which classes give a structure any
allowance for its size.

The seventeen handler vtables are named by their own RTTI. Slot +0xc is
530580 (the base test) for WeaponType and every Line-of-Sight, Guided, Remote
Effect and Wandering class, 52bc60 for BallisticWeapon (its arc, then
530580), 532290 for DroppedBallisticWeapon (a bare call of 530580) and 52b980
for MeleeWeapon (footprint boxes, never `range`). Each is called with a
shooter (2x2) and a target building (7x12, an Aramon Barracks; our old pad
for it was 8*12+24 = 120 px) at centre distances around the weapon's range,
along x and on a diagonal. The model: every class but melee is in range
exactly when the centre distance is within `range` (inclusive), whatever the
target's footprint; a Ballistic shell whose arc reaches is no different.
Melee answers by its boxes alone and is reported, not modelled.

    python3 tools/re/check_inrange_classes.py
"""
import math
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
game, shooter, target, data, handler = [HEAP + i * 0x20000 for i in range(5)]

HANDLERS = {
    0x5f36dc: 'BallisticWeapon', 0x5f3710: 'WeaponType', 0x5f374c: 'LineOfSightLightning',
    0x5f3780: 'LineOfSightFire', 0x5f37e4: 'GuidedWeapon', 0x5f3840: 'RemoteEffectWeapon',
    0x5f3880: 'HailstormWeapon', 0x5f38f4: 'WanderingWeapon', 0x5f3934: 'AreaTurnToFrozenWeapon',
    0x5f3974: 'AreaMindControlWeapon', 0x5f39b4: 'EarthquakeWeapon', 0x5f39f4: 'LineOfSightWeapon',
    0x5f3a28: 'LineOfSightTurnToFrozen', 0x5f3a5c: 'LineOfSightTurnToStone',
    0x5f3a90: 'LineOfSightMindControl', 0x5f3ac4: 'DroppedBallisticWeapon', 0x5f3af8: 'MeleeWeapon',
}


def put(a, v, fmt='I'):
    p.uc.mem_write(a, struct.pack('<' + fmt, v))


def dword(a):
    return struct.unpack('<I', p.uc.mem_read(a, 4))[0]


def rtti(vtable):
    descriptor = dword(dword(vtable - 4) + 12)
    return bytes(p.uc.mem_read(descriptor + 8, 64)).split(b'\0')[0].decode()


put(0x62d55c, game)
put(game + 0x19ecc, 8155)          # map gravity (a shipped map's)
RANGE, PAD = 300, 8 * 12 + 24
# A fast shell (2000 px/s): its arc reaches every distance tried, so only
# the base test can say no.
raw = int(2000.0 * 2184.5333333333333)
steps = max(1, (raw + 0xfffff) >> 20)
put(data + 0x90, RANGE)
put(data + 0x94, 0)
put(data + 0xc8, 0)
put(data + 0xcc, raw // steps)
put(data + 0xd0, steps)
put(handler + 4, 0)
put(handler + 8, 1.0, 'f')
for base, foot in ((shooter, (2, 2)), (target, (7, 12))):
    put(base + 0x130, 0)
    p.uc.mem_write(base + 0x78, struct.pack('<2h', *foot))

dists = [RANGE - 1, RANGE, RANGE + 1, RANGE + 8, RANGE + 40, RANGE + PAD - 1, RANGE + PAD]
predicates, bad, rows = {}, [], 0
print(f'range {RANGE}, target 7x12 (old pad {PAD} px); in-range answers at centre distance:')
print(f'{"class":26s} {"+0xc":>7s}  ' + ' '.join(f'{d:>4d}' for d in dists) + '   diagonal')
for vtable, name in sorted(HANDLERS.items(), key=lambda kv: kv[1]):
    assert rtti(vtable)[4:].startswith(name + '@@'), (hex(vtable), rtti(vtable))
    predicate = dword(vtable + 0xc)
    predicates.setdefault(predicate, []).append(name)
    put(handler, vtable)
    line, diag = [], []
    for along in (True, False):
        for d in dists:
            dx, dz = (d, 0) if along else (d / math.sqrt(2), d / math.sqrt(2))
            sx, sz = 1000 << 16, 1000 << 16
            tx, tz = sx + int(round(dx * 65536)), sz + int(round(dz * 65536))
            p.uc.mem_write(shooter + 0x68, struct.pack('<3i', sx, 0, sz))
            p.uc.mem_write(target + 0x68, struct.pack('<3i', tx, 0, tz))
            p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
            value, error = p.call(predicate, (shooter, target, data), ecx=handler)
            if error:
                raise RuntimeError(f'{name}: {error}')
            got = 1 if value & 0xff else 0
            rows += 1
            (line if along else diag).append(got)
            if name != 'MeleeWeapon':
                cx, cz = tx - sx, tz - sz
                centre = ((cx * cx) >> 32) + ((cz * cz) >> 32) <= RANGE * RANGE
                if got != centre:
                    bad.append((name, d, along, got))
    print(f'{name:26s} {predicate:7x}  ' + ' '.join(f'{g:>4d}' for g in line) + '   ' +
          ''.join(str(g) for g in diag))
for predicate, names in sorted(predicates.items()):
    print(f'  {predicate:x}: {", ".join(sorted(names))}')
assert not bad, bad[:8]
print(f'PASS: {rows} native answers; every class but MeleeWeapon is in range exactly at '
      f'centre distance <= {RANGE}, none within the {PAD} px footprint allowance')
