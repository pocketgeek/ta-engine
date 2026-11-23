#!/usr/bin/env python3
"""Observe retail's weapon SET handshake and aim/reload readiness gate.

Geometry is substituted with controlled angles; mana cost, script setters and
readiness branching execute natively. This establishes controller semantics,
not parity of the current display controller or authoritative firing schedule.

The last part runs 52fe30's unit-target branch (51a9a0 answers a unit) through
every weapon handler class's real vtable: its +0x14 picks the point the body
faces, 51b340 -> 4dc0b0 post the request on the mover, and 52fff0 gates on it.
The led SweetSpot (51aa50) and the range predicates are controlled.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESP

p = Icd()
unit, kind, weapon, script, host, mover = [HEAP + i * 0x10000 for i in range(6)]
target, handler, navigator, navigator_vtable, notify_stub = [HEAP + i * 0x10000 for i in range(8, 13)]
record = unit + 12
angles = (0, 0)
geometry_calls = 0
callbacks = []
rows, expected_rows = [], []
target_ref = 0      # 51a9a0's answer: 0 takes the ground-point branch
led = (0, 0, 0)     # 51aa50's controlled led SweetSpot
notified = 0

def put(address, value, fmt='I'):
    p.uc.mem_write(address, struct.pack('<' + fmt, value))

def word(address):
    return struct.unpack('<H', p.uc.mem_read(address, 2))[0]

def geometry(uc, sp):
    global geometry_calls
    source, state = struct.unpack('<2I', uc.mem_read(sp, 8))
    assert source == unit and state == record
    uc.mem_write(state + 0x16, struct.pack('<2H', *angles))
    geometry_calls += 1
    return 2, 0

def target_point(uc, sp):
    source, output, slot = struct.unpack('<3I', uc.mem_read(sp, 12))
    assert source == unit
    uc.mem_write(output, struct.pack('<3i', *led))
    return 3, 1

def notify(uc, sp):
    global notified
    notified += 1
    return 0, 0

def callback(uc, sp):
    args = struct.unpack('<8I', uc.mem_read(sp, 32))
    name = bytes(uc.mem_read(args[0], 64)).split(b'\0')[0].decode('ascii')
    callbacks.append((name, args[3], args[4:4 + args[3]]))
    return 8, 0

p.hooks[0x52fd80] = geometry
p.hooks[0x51a9a0] = lambda uc, sp: (2, target_ref)
p.hooks[0x51aa50] = target_point
p.hooks[0x530630] = lambda uc, sp: (3, 0)
p.hooks[HEAP + 0x70000] = lambda uc, sp: (3, 1)
p.hooks[0x56c640] = callback
p.hooks[0x4ea4f0] = lambda uc, sp: (4, 0)
# The unit-target range predicates (vtable +0xc) of every handler class pass.
for predicate in (0x530580, 0x52bc60, 0x532290, 0x52b980):
    p.hooks[predicate] = lambda uc, sp: (3, 1)
p.hooks[notify_stub] = notify  # the navigator's facing-request notification (+0x38)
p.freeze_hooks()
put(unit + 0xb4, kind)
put(unit + 0xf4, 1.0, 'f')  # Mana-cost multiplier, avoiding unrelated rank lookup.
put(record, weapon)
put(script + 0xa64, host)
put(host + 12, unit)
rng = random.Random(0x52fff0)

# SET 21 clears the pending aim bits; SET 22/23 independently acknowledge bits.
for _ in range(4096):
    slot = rng.randrange(3)
    address = unit + 12 + slot * 28 + 0x1a
    flags = rng.randrange(65536)
    opcode = rng.choice((21, 22, 23))
    put(address, flags, 'H')
    put(unit + 0xd0, 0)
    _, error = p.call(0x50d450, (opcode, slot), ecx=script)
    assert not error, error
    expected = flags & 0xff07 if opcode == 21 else flags | (8 if opcode == 22 else 16)
    assert word(address) == expected, (opcode, flags, word(address), expected)
    rows.append(f'{opcode} 0 0 {flags} 0 0 0 0 0')
    expected_rows.append((0, 0, 0, expected))
    assert struct.unpack('<I', p.uc.mem_read(unit + 0xd0, 4))[0] == 4

# Full aim-start routine, with valid target/range predicates controlled.
put(weapon, HEAP + 0x60000)
put(HEAP + 0x60000 + 0x10, HEAP + 0x70000)
for _ in range(4096):
    old = (rng.randrange(65536), rng.randrange(65536))
    angles = (rng.randrange(65536), rng.randrange(65536))
    flags = rng.randrange(65536)
    p.uc.mem_write(record + 0x16, struct.pack('<3H', *old, flags))
    callbacks.clear()
    geometry_calls = 0
    result, error = p.call(0x52fe30, (unit, record), ecx=weapon)
    assert not error, error
    start = not (flags & 0xe0)
    expected_flags = ((flags & ~8) | 0xe0) if start else flags
    expected_angles = angles if start else old
    assert result == 1
    assert word(record + 0x1a) == expected_flags
    assert tuple(struct.unpack('<2H', p.uc.mem_read(record + 0x16, 4))) == expected_angles
    assert geometry_calls == int(start)
    assert callbacks == ([('AimWeapon', 3, (*angles, flags & 3))] if start else [])
    rows.append(f'0 {old[0]} {old[1]} {flags} {angles[0]} {angles[1]} 0 0 0')
    expected_rows.append((int(start), *expected_angles, expected_flags))

def distance(a, b):
    return abs(((a - b + 32768) & 65535) - 32768)

for i in range(8192):
    old = (rng.randrange(65536), rng.randrange(65536))
    tolerance = rng.randrange(65536)
    # Exercise exact tolerance boundaries as well as arbitrary wrapped angles.
    delta = (tolerance + (i % 3) - 1) if i % 2 else rng.randrange(65536)
    angles = ((old[0] + delta) & 65535,
              (old[1] + (tolerance // 2 if i % 5 else rng.randrange(65536))) & 65535)
    flags = rng.randrange(65536)
    reload_ticks = 1 if i % 13 == 0 else 0
    enough_mana = i % 17 != 0
    aircraft = i % 7 == 0
    has_mover = aircraft or i % 3 != 0
    mobile = i % 4 != 0
    heading = rng.randrange(65536)
    desired_heading = (heading + (i % 1024)) & 65535
    put(unit + 8, mover if has_mover else 0)
    put(kind + 0x260, 0x08000800 if aircraft else 0)
    put(kind + 0x190, int(mobile), 'H')
    put(unit + 0x7e, heading, 'H')
    put(mover + 0x34, desired_heading, 'H')
    put(unit + 0xd8, 10.0 if enough_mana else 0.0, 'f')
    put(weapon + 0xd4, 1.0, 'f')
    put(weapon + 0xc0, tolerance, 'H')
    put(record + 0x14, reload_ticks, 'H')
    p.uc.mem_write(record + 0x16, struct.pack('<3H', *old, flags))
    geometry_calls = 0
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    result, error = p.call(0x52fff0, (unit, record), ecx=weapon)
    assert not error, error
    expected_flags = flags
    success = False
    if enough_mana and not reload_ticks:
        changed = distance(angles[0], old[0]) > tolerance or distance(angles[1], old[1]) > tolerance // 2
        if changed:
            if flags & 0xe0:
                expected_flags = (flags & ~0xe0) | ((flags & 0xe0) - 0x20)
        elif flags & 8:
            success = not (has_mover and (mobile or aircraft) and distance(heading, desired_heading) > max(tolerance, 512))
    assert result == int(success), (i, flags, old, angles, result, success)
    assert word(record + 0x1a) == expected_flags, (i, flags, expected_flags)
    assert tuple(struct.unpack('<2H', p.uc.mem_read(record + 0x16, 4))) == (angles if success else old)
    assert geometry_calls == int(enough_mana and not reload_ticks)
    aligned = not (has_mover and (mobile or aircraft) and distance(heading, desired_heading) > max(tolerance, 512))
    rows.append(f'1 {old[0]} {old[1]} {flags} {angles[0]} {angles[1]} {tolerance} {int(enough_mana and not reload_ticks)} {int(aligned)}')
    expected_rows.append((int(success), *(angles if success else old), expected_flags))

result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test', '--aim-state'],
                        input='\n'.join(rows)+'\n', text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected_rows, next(((rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected_rows)) if a != b), 'length mismatch')
print('PASS: C++ matches 4096 native SET handshakes, 4096 aim starts and 8192 readiness gates')

# 52fe30's unit-target branch through every weapon handler class. Each class's
# vtable is named by its own RTTI; only MeleeWeapon's +0x14 (531f00) answers
# true and faces the target's origin, every other class (+0x14 42a6b0) faces
# the led SweetSpot. World's Weapon::melee is FBI type=Melee, MeleeWeapon.
HANDLERS = {
    0x5f36dc: 'BallisticWeapon', 0x5f3710: 'WeaponType', 0x5f374c: 'LineOfSightLightning',
    0x5f3780: 'LineOfSightFire', 0x5f37e4: 'GuidedWeapon', 0x5f3840: 'RemoteEffectWeapon',
    0x5f3880: 'HailstormWeapon', 0x5f38f4: 'WanderingWeapon', 0x5f3934: 'AreaTurnToFrozenWeapon',
    0x5f3974: 'AreaMindControlWeapon', 0x5f39b4: 'EarthquakeWeapon', 0x5f39f4: 'LineOfSightWeapon',
    0x5f3a28: 'LineOfSightTurnToFrozen', 0x5f3a5c: 'LineOfSightTurnToStone',
    0x5f3a90: 'LineOfSightMindControl', 0x5f3ac4: 'DroppedBallisticWeapon', 0x5f3af8: 'MeleeWeapon',
}
def dword(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]
for vtable, name in HANDLERS.items():
    descriptor = dword(dword(vtable - 4) + 12)
    assert bytes(p.uc.mem_read(descriptor + 8, 64)).split(b'\0')[0] == f'.?AU{name}@@'.encode(), name

put(mover, navigator)
put(navigator, navigator_vtable)
put(navigator_vtable + 0x38, notify_stub)
target_ref = target
posts = []  # 4dc0b0 runs natively; observe its heading argument on entry
def observe_post(uc, address, size, _):
    posts.append(struct.unpack('<H', uc.mem_read(uc.reg_read(UC_X86_REG_ESP) + 4, 2))[0])
p.uc.hook_add(UC_HOOK_CODE, observe_post, begin=0x4dc0b0, end=0x4dc0b0)
rows, expected_rows = [], []
def coordinate():
    return rng.randrange(0, 4000 * 65536)
for i in range(8192):
    vtable = list(HANDLERS)[i % len(HANDLERS)]
    melee = HANDLERS[vtable] == 'MeleeWeapon'
    put(handler, vtable)
    can_fly = i % 5 == 0
    hover = can_fly and rng.randrange(2) == 1
    has_mover = can_fly or i % 7 != 0
    pivot = 0 if i % 3 == 0 else rng.randrange(1, 65536)
    put(unit + 8, mover if has_mover else 0)
    put(kind + 0x260, (0x800 if can_fly else 0) | (0x8000000 if hover else 0))
    put(kind + 0x190, pivot, 'H')
    origin = [coordinate() for _ in range(3)]
    target_origin = [coordinate() for _ in range(3)]
    led = tuple(coordinate() for _ in range(3))
    if i % 11 == 0:  # the point straight above or below the unit
        led = (origin[0], led[1], origin[2])
        target_origin[0], target_origin[2] = origin[0], origin[2]
    if i % 13 == 0:  # one axis aligned
        led = (origin[0], led[1], led[2])
    p.uc.mem_write(unit + 0x68, struct.pack('<3i', *origin))
    p.uc.mem_write(target + 0x68, struct.pack('<3i', *target_origin))
    old_request, old_flags = rng.randrange(65536), rng.randrange(65536)
    put(mover + 0x34, old_request, 'H')
    put(mover + 0x36, old_flags, 'H')
    # Most records are aiming already (no aim start) and acknowledged (bit 8),
    # so the gate reaches the facing test; some start aiming afresh.
    flags = rng.randrange(65536)
    if i % 4:
        flags |= 8 | (0x20 << rng.randrange(3))
    angles = (rng.randrange(65536), rng.randrange(65536))
    p.uc.mem_write(record + 0x16, struct.pack('<3H', *angles, flags))
    tolerance = rng.randrange(65536) if i % 2 else rng.randrange(1024)
    put(weapon + 0xc0, tolerance, 'H')
    put(record + 0x14, 0, 'H')
    put(unit + 0xd8, 10.0, 'f')
    put(weapon + 0xd4, 1.0, 'f')
    notified = 0
    posts.clear()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    result, error = p.call(0x52fe30, (unit, record), ecx=handler)
    assert not error and result == 1, (i, error, result)
    request, mover_flags = word(mover + 0x34), word(mover + 0x36)
    assert len(posts) <= 1 and (not posts or posts[0] == request), (i, posts, request)
    # 4dc0b0 stores and notifies the navigator only when the request changes.
    assert notified == int(bool(posts) and (not old_flags & 0x10 or request != old_request)), i
    # Face the body near the request: on, inside, at and just past the bound.
    bound = max(tolerance, 512)
    heading = (request + rng.choice((0, bound - 1, bound, bound + 1, -bound, -bound - 1,
                                     rng.randrange(65536)))) & 0xffff
    put(unit + 0x7e, heading, 'H')
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    ready, error = p.call(0x52fff0, (unit, record), ecx=handler)
    assert not error, (i, error)
    rows.append(' '.join(map(str, [origin[0], origin[2], target_origin[0], target_origin[2], led[0], led[2],
                                   int(melee), int(has_mover), int(can_fly), int(hover), pivot,
                                   old_request, old_flags, heading, tolerance, flags, *angles])))
    expected_rows.append((int(bool(posts)), request, mover_flags, ready, word(record + 0x1a)))

result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_visual_test', '--aim-facing'],
                        input='\n'.join(rows)+'\n', text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected_rows, next(((rows[i], a, b) for i, (a, b) in enumerate(zip(actual, expected_rows)) if a != b), 'length mismatch')
faced = sum(1 for row in expected_rows if row[0])
print(f'PASS: C++ matches 8192 native unit-target aim starts and gates over all {len(HANDLERS)} handler classes '
      f'({faced} facing requests; melee faces the target origin, every other class the led SweetSpot)')
