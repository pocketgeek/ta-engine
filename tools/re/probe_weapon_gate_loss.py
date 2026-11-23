#!/usr/bin/env python3
"""Exercise native target-range admission, retirement, and SET-23 release.

The real 52ae90, 51a9a0, 5306a0, 530580, 52fe30 and 530140 execute. Only the
weapon geometry, aim-readiness result, display transport, projectile sink, and
the no-target result after 51a9a0 retires a dead reference are controlled. The
probe covers the common update for live and retired targets; it does not emulate
the mission caller or retail fog map.
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
game, units, kind, weapon_type, script, host = [HEAP + i * 0x10000 for i in range(6)]
weapon_object = HEAP + 0x70000
shooter, target = units + 312, units + 624
record = shooter + 12
trace = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def short(address, value):
    p.uc.mem_write(address, struct.pack('<H', value & 0xffff))


def word(address):
    return struct.unpack('<H', p.uc.mem_read(address, 2))[0]


def fixed(address, value):
    p.uc.mem_write(address, struct.pack('<i', int(value * 65536)))


def geometry(_uc, _sp):
    # Avoid unrelated script-piece lookup while retaining native range gates.
    short(record + 0x16, 0)
    short(record + 0x18, 0)
    trace.append(('geometry',))
    return 2, 0


def target_point(uc, sp):
    source, output, slot = struct.unpack('<3I', uc.mem_read(sp, 12))
    assert source == shooter and slot == 0
    # The native target-retirement routine below clears this reference before
    # the next common update. Model its resulting "no target" target-point
    # result while leaving the controller, readiness and release gates native.
    if struct.unpack('<H', uc.mem_read(record + 4, 2))[0] == 0:
        trace.append(('target_point_no_target',))
        return 3, 0
    uc.mem_write(output, bytes(12))
    trace.append(('target_point',))
    return 3, 1


def callback(uc, sp):
    args = struct.unpack('<8I', uc.mem_read(sp, 32))
    name = bytes(uc.mem_read(args[0], 64)).split(b'\0')[0].decode('ascii')
    trace.append((name, args[3], args[4:4 + args[3]]))
    if name == 'AimWeapon':
        # Model an immediate COB SET 22 so the common updater proceeds to
        # its separately probed native readiness gate.
        short(record + 0x1a, word(record + 0x1a) | 8)
    return 8, 0


def aim_display(_uc, _sp):
    return 4, 0


def fire_display(_uc, _sp):
    return 2, 0


def ready(_uc, _sp):
    trace.append(('ready',))
    # Readiness arithmetic is independently exercised by check_animation_readiness.py.
    return 2, 1


def projectile(_uc, sp):
    args = struct.unpack('<2I', _uc.mem_read(sp, 8))
    assert args == (shooter, record), args
    trace.append(('projectile',))
    short(record + 0x1a, word(record + 0x1a) & 0xff0f)
    return 2, 0


def zero_random(_uc, _sp):
    return 0, 0


p.hooks.update({
    0x52fd80: geometry,
    0x51aa50: target_point,
    0x52fff0: ready,
    0x56c640: callback,
    0x4ea4f0: aim_display,
    0x4ea560: fire_display,
    0x530220: projectile,
    0x5d4444: zero_random,
})
p.freeze_hooks()

# Valid native unit table and one live target at id 2. This leaves 51a9a0's
# actual target ref/alive checks and the real WeaponType vtable range test on.
put(0x62d55c, game)
put(game + 0x14e84, units)
put(game + 0x14e88, units + 4 * 312)
# Retail's active per-player visibility grid is game+0x19ef4. Filling the
# whole fixture grid with zero makes this target unseen while keeping its
# authoritative unit-table reference alive.
visibility = HEAP + 0x80000
put(game + 0x19ef4, visibility)
p.uc.mem_write(visibility, bytes(4096))
put(shooter + 0xb4, kind)
put(shooter + 0xbc, script)
p.uc.mem_write(shooter + 0xf4, struct.pack('<f', 1.0))  # bypass unrelated rank lookup
put(script + 0xa64, host)
put(host + 12, shooter)
put(kind + 0x260, 0x10000)
put(shooter + 0x130, 0)  # selected weapon slot 0
put(record, weapon_type)
put(weapon_type + 0x40, weapon_object)
put(weapon_object, 0x5f3710)  # base WeaponType; +0xc is native 530580
put(weapon_type + 0x90, 400)
put(weapon_type + 0x94, 0)
put(weapon_type + 0xc8, 0)
put(weapon_type + 0x9c, 30)
p.uc.mem_write(weapon_type + 0xd4, struct.pack('<f', 1.0))
put(target + 0x130, 0x1000000)
p.uc.mem_write(record + 4, struct.pack('<2H', 2, 0x8000))
for body, x, z in ((shooter, 200, 200), (target, 300, 200)):
    fixed(body + 0x68, x)
    fixed(body + 0x70, z)
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)

# Losing current vision does not retire an already-assigned live target in
# retail's native lookup. Selection/mission target acquisition is outside this
# probe; the test starts from an existing weapon target reference.
p.uc.mem_write(visibility, bytes([2]) * 4096)
result, error = p.call(0x51a9a0, (shooter, 0))
assert not error and result == target, (result, error)
p.uc.mem_write(visibility, bytes(4096))
result, error = p.call(0x51a9a0, (shooter, 0))
assert not error and result == target, (result, error)
assert not trace, trace
print('PASS: native 51a9a0 retains an assigned live target across a visible-to-hidden FOW edge')

# Range admission is inclusive: 400 is admitted, 401 is rejected.
for distance, expected in ((400, True), (401, False)):
    fixed(target + 0x68, 200 + distance)
    short(record + 0x14, 0)
    short(record + 0x1a, 0)
    trace.clear()
    _, error = p.call(0x52ae90, (shooter,))
    assert not error, error
    callbacks = [event[0] for event in trace if event[0] in ('AimWeapon', 'FireWeapon')]
    assert (callbacks == ['AimWeapon', 'FireWeapon']) == expected, (distance, trace)
    assert (('ready',) in trace) == expected, (distance, trace)
    print(f'PASS: native 52ae90 at {distance}px (range 400): {"Aim/ready/Fire callbacks" if expected else "no Aim/ready/Fire callbacks"}')

# A zeroed current-visibility grid does not invalidate an otherwise-live
# target in this common weapon update: it has no direct fog lookup.
fixed(target + 0x68, 300)
short(record + 0x14, 0)
short(record + 0x1a, 0)
trace.clear()
_, error = p.call(0x52ae90, (shooter,))
assert not error, error
assert [event[0] for event in trace if event[0] in ('AimWeapon', 'FireWeapon')] == [
    'AimWeapon', 'FireWeapon'
], trace
print('PASS: a live in-range target behind an all-hidden FOW grid still reaches native AimWeapon/FireWeapon callbacks')

# SET 23 may mature after the target moves out of weapon range. Unlike the
# ready-false case (which still reaches the bit test), AimStart=false branches
# around the SET-23 check too. The signal waits until range admission resumes.
fixed(target + 0x68, 1000)
short(record + 0x14, 10)
short(record + 0x1a, 0xe8 | 16)
trace.clear()
_, error = p.call(0x52ae90, (shooter,))
assert not error, error
assert not any(event[0] in ('AimWeapon', 'FireWeapon', 'ready', 'projectile') for event in trace), trace
assert word(record + 0x1a) & 16
print('PASS: out-of-range target skips AimWeapon/readiness/FireWeapon/projectile and retains pending SET 23')

fixed(target + 0x68, 300)
short(record + 0x14, 0)
trace.clear()
_, error = p.call(0x52ae90, (shooter,))
assert not error, error
assert ('projectile',) in trace, trace
assert not (word(record + 0x1a) & 16)
print('PASS: returning in range resumes the pending SET-23 projectile release')

# A pending release belongs to its assigned target. Retail first retires a
# dead unit reference in 51a9a0; the following common update must not release
# the saved SET-23 acknowledgement without a target.
put(target + 0x130, 0x1000000)
p.uc.mem_write(record + 4, struct.pack('<2H', 2, 0x8000))
fixed(target + 0x68, 1000)
short(record + 0x14, 10)
short(record + 0x1a, 0xe8 | 16)
trace.clear()
_, error = p.call(0x52ae90, (shooter,))
assert not error, error
assert not any(event[0] in ('ready', 'FireWeapon', 'projectile') for event in trace), trace
assert word(record + 0x1a) & 16

put(target + 0x130, 0)
trace.clear()
result, error = p.call(0x51a9a0, (shooter, 0))
assert not error, error
assert result == 0
assert struct.unpack('<2H', p.uc.mem_read(record + 4, 4)) == (0, 0x8000)
assert word(record + 0x1a) & 16
assert [event[0] for event in trace] == ['TargetCleared'], trace
print('PASS: native target retirement clears a dead target and preserves its pending SET 23')

trace.clear()
_, error = p.call(0x52ae90, (shooter,))
assert not error, error
assert ('target_point_no_target',) in trace
assert not any(event[0] in ('ready', 'FireWeapon', 'projectile') for event in trace), trace
assert word(record + 0x1a) & 16
print('PASS: native common update cannot release the pending shot after target retirement')
