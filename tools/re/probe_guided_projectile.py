#!/usr/bin/env python3
"""Compare native GuidedWeapon launch/XYZ steering with the deterministic port.

The source muzzle, current target SweetSpot and per-substep collision result are
controlled sinks. Native 0x52c540, 0x52e030 and 0x52c6d0 remain executable;
the probe compares their fixed-point XYZ/BAM records with retail_visual_test's
guided-projectile mode. No retail GUI is launched.

After the random tracking cases come the updates that skip steering: no target
(+0x78 null), a dead target (+0x130 without 0x1000000), a dying one (0x1000)
and a target dead ahead (zero turn angle). World passes the shot's own position
for the first three, as it does for a dead target or a point shot. A modelled
shot (+0x93) must still rebuild yaw/pitch from its velocity (0x52e23d).

Three more modes follow (2026-11-04):
- near / nudge: a target dead ahead, or within a few raw units of dead ahead,
  at a NON-axis-aligned direction. 0x5ad930 takes acos(dot/(|a||b|)) with the
  cosine stored as a float, so whether native turns (and so snaps to
  substeps*speed) here depends on that cosine's rounding, not on the true angle.
  With a zero turn rate native makes a turn by 0, and onto a zero axis (the
  target exactly in line) that stops the shot.
- carried: the target is first put aboard a host by the native attach 0x51b5a0
  (its +0x130 alive/dying bits are kept), its carried bits (+0x114 0x14) are set,
  and its +0x68 is the host's point (0x4dad30's commit for attach piece -1, see
  check_cargo_attach_position.py). 0x52e030 still steers at it.
The target's SweetSpot is NOT a substitute: the native 0x4dd4f0 -> 0x4dd2a0
run for every mode over the target's own +0x68 and a drawn piece buffer, with
only the script query 0x56c720 answered here (piece 2).
"""
import random
import struct
import subprocess
import sys

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


def put(uc, address, value):
    uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def putf(uc, address, value):
    uc.mem_write(address, struct.pack('<f', value))


def signed(value):
    return struct.unpack('<i', struct.pack('<I', value & 0xffffffff))[0]


def i32s(uc, address, count=3):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, 4 * count))


def u16s(uc, address, count=3):
    return struct.unpack('<' + 'H' * count, uc.mem_read(address, 2 * count))


def ptr(uc, address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]


p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)  # Native converter temporarily truncates float-to-int.
game, data, guided, shot, owner, target, aim, unit_type, art, script, tdraw, tmesh, tverts, scratch = [
    HEAP + i * 0x30000 for i in range(1, 15)]
# The attach looks units up by id in the game's unit pool (+0x14e84, 0x138 per
# unit): the target is id 1 and its host id 2, right after it.
pool = target - 0x138
host = target + 0x138
sweet_piece = 2
sweet_offset = (0, 0, 0)
origin = (0, 0, 0)
target_point = (0, 0, 0)
slot = 0
collision_at = 0
collision_calls = 0
impact_events = 0
collision_rows = []


def muzzle(uc, sp):
    args = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert args == (owner, shot + 4, slot, 0xffffffff), args
    uc.mem_write(args[1], struct.pack('<3i', *origin))
    return 4, 0


def veteran(uc, sp):
    assert ptr(uc, sp) == owner
    return 1, 0


def buildup(uc, sp):
    return 3, 0


def script_query(uc, sp):
    # 0x4dd4f0 asks the target's script (+0xbc) which piece is its SweetSpot.
    from unicorn.x86_const import UC_X86_REG_ECX
    args = struct.unpack('<5I', uc.mem_read(sp, 20))
    assert uc.reg_read(UC_X86_REG_ECX) == scratch and args[0] == 0x615c30 and args[1] != 0, args
    put(uc, args[1], sweet_piece)
    return 5, 0


def no_args(count):
    return lambda uc, sp: (count, 0)


def collision(uc, sp):
    global collision_calls
    assert ptr(uc, sp) == shot
    collision_calls += 1
    collision_rows.append((i32s(uc, shot + 4), i32s(uc, shot + 0x1c), u16s(uc, shot + 0x34)))
    return 1, 2 if collision_at and collision_calls == collision_at else 0


def impact(uc, sp):
    global impact_events
    values = struct.unpack('<5I', uc.mem_read(sp, 20))
    assert values == (shot, 123, 0, 1, 0), values
    impact_events += 1
    return 5, 0


p.hooks.update({0x4dd420: muzzle, 0x56c720: script_query, 0x519310: veteran,
                0x537390: buildup, 0x52a4d0: collision, 0x529c10: impact,
                0x5065f0: no_args(1), 0x4b3c80: no_args(0)})
p.freeze_hooks()
put(p.uc, 0x62d55c, game)
put(p.uc, game + 0x19f44, 100)
put(p.uc, owner + 0xb4, unit_type)
put(p.uc, owner + 0xc0, script)
put(p.uc, owner + 0x130, 0x01000000)
put(p.uc, target + 0x130, 0x01000000)
put(p.uc, unit_type + 0x8a, art)
put(p.uc, aim, data)
put(p.uc, shot + 0x80, 123)
put(p.uc, shot + 0x7c, owner)
put(p.uc, shot + 0x78, target)
put(p.uc, guided, 0x5f37e4)
put(p.uc, game + 0x14e84, pool)
put(p.uc, game + 0x14e88, host)
put(p.uc, target + 0xbc, scratch)
put(p.uc, target + 0xc0, tdraw)
put(p.uc, tdraw + 0x1cc + sweet_piece * 0x38, tmesh)
put(p.uc, tdraw + 0x1cc + sweet_piece * 0x38 + 0x24, tverts)
put(p.uc, tmesh + 4, 1)
for unit_id, unit in ((1, target), (2, host)):
    p.uc.mem_write(unit + 2, struct.pack('<H', unit_id))
    put(p.uc, unit + 8, scratch + 0x100 + unit_id * 0x40)    # mover
    put(p.uc, unit + 0xb8, scratch + 0x200 + unit_id * 0x100)  # no drawable sound object


def place_target(point):
    """Put the target so that native 0x4dd2a0 answers `point`: its own +0x68 plus
    the drawn bounds centre of one vertex (bounds start at 0, so the centre is
    half the vertex)."""
    global sweet_offset
    vertex = tuple(2 * rng.randrange(-40 * 65536, 40 * 65536) for _ in range(3))
    sweet_offset = tuple(v // 2 for v in vertex)
    p.uc.mem_write(tverts, struct.pack('<3i', *vertex))
    origin_point = tuple(signed(point[i] - sweet_offset[i]) for i in range(3))
    if carried:
        put_xyz(host + 0x68, origin_point)       # the host carries it here
        put_xyz(target + 0x68, i32s(p.uc, host + 0x68))  # 4dad30's commit, piece -1
    else:
        put_xyz(target + 0x68, origin_point)


def put_xyz(address, value):
    p.uc.mem_write(address, struct.pack('<3i', *value))


def attach_target():
    """Run the native attach 0x51b5a0 (packet 7: cargo 1, host 2, piece 0xff)."""
    for unit in (target, host):
        for offset in (0xa8, 0xac, 0xb0):
            put(p.uc, unit + offset, 0)
    put(p.uc, host + 0x130, 0x01000000)
    put(p.uc, target + 0x130, 0x01000010)
    packet = scratch + 0x800
    p.uc.mem_write(packet, bytes((7, 1, 0, 2, 0, 0xff, 1, 0)))
    _, error = p.call(0x51b5a0, (packet, 0))
    assert error is None, error
    assert ptr(p.uc, target + 0xa8) == host and ptr(p.uc, host + 0xac) == target
    flags = ptr(p.uc, target + 0x130)
    assert flags & 0x01001000 == 0x01000000 and flags & 0x8000, hex(flags)
    put(p.uc, target + 0x114, ptr(p.uc, target + 0x114) | 0x14)  # carried bits
    return flags

carried = False
carried_flags = set()

rng = random.Random(0x52e03052c6d0)
rows = []
expected = []
mode_rows = []
cases = 384
modes = ('none', 'dead', 'dying', 'ahead', 'near', 'nudge', 'carried')
special = 48 * len(modes)
binary = sys.argv[1] if len(sys.argv) > 1 else 'build/retail_visual_test'
for case in range(cases + special):
    mode = 'track' if case < cases else modes[case % len(modes)]
    # Include flat ground, flyers with positive/negative vertical separation,
    # straight/vertical headings, moving SweetSpots, and exact/near turn caps.
    origin = tuple(rng.randrange(-3000, 3000) * 65536 for _ in range(3))
    body = rng.randrange(65536)
    heading = rng.choice((0, 0x4000, 0x8000, 0xc000, rng.randrange(65536)))
    pitch = 0 if case % 9 == 0 else rng.choice((0, 0x1000, 0x4000, 0xc000, rng.randrange(65536)))
    speed = rng.choice((0x100000, 0x400000, 0x800000, rng.randrange(0x20000, 0x900000)))
    steps = rng.choice((1, 2, 3, 4, 7))
    turn = rng.choice((0.0, 0.002, 0.05, 0.2, 1.2, rng.random() * 0.45))
    if case in (1, 2):
        # Current velocity points +Z and the target lies exactly on -Z.
        # The cross axis is zero while dot is negative (the antiparallel edge).
        body, heading, pitch, turn = 0, 0, 0, 0.2
    update_angles = case % 2 == 0 if mode == 'track' else case % 3 != 0
    if case in (1, 2):
        # Keep the native zero-axis antiparallel case with refresh disabled;
        # refreshing its BAM direction takes a native degenerate quaternion
        # path (0x2023bc) and faults before the base movement routine.
        update_angles = case == 2
    if mode in ('near', 'nudge'):
        # A launch off every axis, so the velocity has three non-zero parts.
        body, heading = 0, rng.randrange(0x400, 0x3c00) + rng.choice((0, 0x4000, 0x8000, 0xc000))
        pitch = rng.choice((rng.randrange(0x200, 0x3000), (-rng.randrange(0x200, 0x3000)) & 0xffff))
        # Turn rate 0 too (a Lightning Ball has none): a turn by 0 that native
        # still makes, onto a zero axis when the target is exactly in line.
        turn = rng.choice((0.0, 0.002, 0.05, 0.1047, 0.2, 0.35, rng.random() * 0.45 + 0.001))
        # The zero-axis turn's BAM refresh takes the degenerate quaternion
        # path (0x2023bc) and faults, as for the antiparallel case.
        update_angles = case % 3 != 0 and turn != 0.0
    if mode == 'ahead':
        # An axis-aligned launch. Native 5ad930 takes acos of the float
        # dot/(|a||b|), which is usually exactly 1 here, but not always: a
        # float dot that rounds below |a||b| still snaps (near/nudge below).
        body, heading, pitch = 0, rng.choice((0, 0x4000, 0x8000, 0xc000)), rng.choice((0, 0x4000, 0xc000))
    putf(p.uc, guided + 4, turn)
    put(p.uc, data + 0xcc, speed)
    put(p.uc, data + 0xd0, steps)
    spin = tuple(rng.randrange(-8, 9) & 0xffff for _ in range(3))
    for offset, value in zip((0xb8, 0xba, 0xbc), spin):
        p.uc.mem_write(data + offset, struct.pack('<H', value))
    put(p.uc, data + 0x48, 111 if update_angles else 0)  # +93 gates yaw/pitch refresh
    put(p.uc, data + 0x4c, 0)
    put(p.uc, data + 0x50, 0)
    put(p.uc, data + 0x54, 0)
    put(p.uc, data + 0x58, 0)
    put(p.uc, data + 0x90, 5000)
    put(p.uc, data + 0xc8, 0)
    put(p.uc, owner + 0x7e, body)
    put(p.uc, aim + 0x16, heading)
    put(p.uc, aim + 0x18, pitch)
    put(p.uc, aim + 0x1a, slot)

    # A stable nominal shape plus vertical and lateral offsets; targets then
    # move independently between updates, as queryUnitScriptPoint does in World.
    dx = rng.choice((0, 300, 900, 2400, 6000))
    dy = rng.choice((0, -2400, -600, 600, 2400))
    dz = rng.choice((0, 300, 900, 2400, 6000))
    if dx == dz == 0 and dy == 0:
        dz = 900
    target_point = tuple(origin[i] + (dx, dy, dz)[i] for i in range(3))
    p.uc.mem_write(shot, bytes(0xb0))
    put(p.uc, shot, data)
    put(p.uc, shot + 0x78, 0 if mode == 'none' else target)
    put(p.uc, target + 0x130, {'dead': 0, 'dying': 0x01001000}.get(mode, 0x01000000))
    carried = mode == 'carried'
    if carried:
        carried_flags.add(attach_target())
    else:
        put(p.uc, target + 0x114, 0)
    put(p.uc, shot + 0x7c, owner)
    put(p.uc, shot + 0x80, 123)
    origin_saved = origin
    _, error = p.call(0x52c540, (shot, owner, aim), ecx=guided)
    assert error is None, (case, error)
    # The common initializer's wrapped range lifetime depends on its weapon-aim
    # wrapper. Give the guided update a controlled live window after validating
    # the actual launch XYZ/velocity/angles above.
    put(p.uc, shot + 0x60, 100)
    put(p.uc, shot + 0x70, 10000)
    initial = (i32s(p.uc, shot + 4), i32s(p.uc, shot + 0x1c), u16s(p.uc, shot + 0x34))
    assert initial[0] == origin_saved, (case, initial, origin_saved)
    if mode == 'ahead':
        assert sum(v != 0 for v in initial[1]) == 1, (case, initial)
    if mode in ('near', 'nudge'):
        assert all(v != 0 for v in initial[1]), (case, initial)
    mode_rows.append(None)
    rows.append(f'L {origin_saved[0]} {origin_saved[1]} {origin_saved[2]} {body} {heading} {pitch} {speed}')
    expected.append((initial, None, None))

    # Two updates exercise moving/vertical tracking. A fraction hit exercises
    # base collision's stop-after-code-2 rule. Code-1 continuation is not
    # exercised by this native harness, whose callback returns only 0 or 2.
    py_state = initial
    for update in range(2):
        delta = (rng.randrange(-900, 6001), rng.choice((0, -1800, -300, 300, 1800)),
                 rng.randrange(-900, 6001))
        if case % 9 == 0 and update == 0:
            delta = (0, rng.choice((-3600, -1200, 1200, 3600)), 0)  # vertical aim
        if case == 1 and update == 0:
            delta = (0, 0, -900)  # exact antiparallel to the +Z launch vector
        elif case == 2 and update == 0:
            delta = (300, 0, -900)  # finite cross axis near antiparallel, refresh enabled
        target_point = tuple(origin_saved[i] + delta[i] * 65536 for i in range(3))
        ported_target = target_point
        if mode in ('none', 'dead', 'dying'):
            ported_target = py_state[0]  # World: no live target -> the shot's own position
        elif mode == 'ahead':
            # Four current velocities ahead along the launch axis.
            target_point = ported_target = tuple(
                signed(py_state[0][i] + 4 * py_state[1][i]) for i in range(3))
        elif mode == 'near':
            # Exactly ahead: cross product exactly 0 in float, the cosine 1 or
            # one float step either side of it.
            ahead = rng.choice((1, 2, 4, 8, 16))
            target_point = ported_target = tuple(
                signed(py_state[0][i] + ahead * py_state[1][i]) for i in range(3))
        elif mode == 'nudge':
            # Keep the point from wrapping: off the int32 range it is no
            # longer ahead (and takes the inexact quaternion turn).
            ahead = min(rng.randrange(2, 60), (1 << 30) // max(1, max(map(abs, py_state[1]))))
            nudge = rng.choice((1, 4, 16, 64, 256))
            target_point = ported_target = tuple(
                signed(py_state[0][i] + ahead * py_state[1][i] + rng.randrange(-nudge, nudge + 1))
                for i in range(3))
        place_target(target_point)
        mode_rows.append(mode)
        collision_calls = impact_events = 0
        collision_rows.clear()
        collision_at = (1 if case % 34 == 0 else max(1, steps // 2)) if case % 17 == 0 and update == 1 else 0
        put(p.uc, game + 0x19f44, 102 + update)
        _, error = p.call(0x52e030, (shot,), ecx=guided)
        assert error is None, (case, update, error, origin_saved, target_point, turn, speed, steps,
            body,heading,pitch,initial,i32s(p.uc,shot+4),i32s(p.uc,shot+0x1c),u16s(p.uc,shot+0x34),
            collision_rows[-1:] if collision_rows else [])
        native = (i32s(p.uc, shot + 4), i32s(p.uc, shot + 0x1c), u16s(p.uc, shot + 0x34))
        rows.append('T ' + ' '.join(map(str, (*py_state[0], *py_state[1], *py_state[2],
            *ported_target, format(turn, '.9g'), speed, steps, *spin, int(update_angles),
            collision_at))))
        expected.append((native, collision_calls, bool(collision_at)))
        if collision_at:
            assert collision_calls == collision_at and impact_events == 1, (case, collision_calls, impact_events)
            assert p.uc.mem_read(shot + 0x97, 1)[0] == 1, case
        else:
            assert collision_calls == steps and impact_events == 0, (case, update, collision_calls, impact_events,
                ptr(p.uc,shot+0x78), ptr(p.uc,shot+0x7c), ptr(p.uc,shot+0x60), ptr(p.uc,shot+0x70),
                struct.unpack('<I',p.uc.mem_read(game+0x19f44,4))[0],struct.unpack('<I',p.uc.mem_read(owner+0x130,4))[0])
        py_state = native
        if collision_at:
            break
        # Current position is the source for next update; native receives the
        # target point explicitly, and only the target continues moving.

ported = subprocess.run([binary, '--guided-projectile'], input='\n'.join(rows) + '\n',
                        text=True, capture_output=True, check=True)
ported_rows = [tuple(map(int, line.split())) for line in ported.stdout.splitlines()]
assert len(ported_rows) == len(expected), (len(ported_rows), len(expected), ported.stderr)
worst = [0] * 9
worst_by_mode = {}
turned = {'near': [0, 0], 'nudge': [0, 0]}
failures = {}  # mode -> [count, first failing row]
for index, (actual, (native, collisions, hit)) in enumerate(zip(ported_rows, expected)):
    want = (*native[0], *native[1], *native[2])
    if collisions is None:
        assert len(actual) == 9, (index, actual)
    else:
        assert len(actual) == 11, (index, actual)
        assert actual[9:] == (collisions, int(hit)), (index, actual, collisions, hit)
    mode = mode_rows[index]
    if mode in turned:
        before = tuple(map(int, rows[index].split()[4:7]))
        turned[mode][0 if native[1] == before else 1] += 1
    for field in range(9):
        if field < 6:
            delta = abs(signed(actual[field] - want[field]))
            worst[field] = max(worst[field], delta)
            if field >= 3 and mode is not None:
                worst_by_mode[mode] = max(worst_by_mode.get(mode, 0), delta)
        else:
            delta = abs(((actual[field] - want[field] + 0x8000) & 0xffff) - 0x8000)
            worst[field] = max(worst[field], delta)
        # An update that skips steering or snaps (0x52e108) is bit-exact; only
        # the quaternion turn (0x52e16b, float axis and fsin/fcos) keeps a few
        # raw units of slack.
        exact = mode in ('none', 'dead', 'dying', 'ahead', 'near', 'nudge')
        if delta > ((0 if exact else 128) if field < 6 else 3):
            entry = failures.setdefault(mode, [0, (index, field, actual, want, delta, rows[index])])
            entry[0] += 1
            break
assert not failures, '\n'.join(f'{mode}: {count} rows, first {first}' for mode, (count, first) in failures.items())
print(f'PASS: {cases + special} native guided launches plus {len(expected)-cases-special} XYZ updates '
      f'({special} launches with no, dead, dying or carried target or a target dead or nearly ahead); '
      f'velocity max raw delta by mode={dict(sorted(worst_by_mode.items()))}; '
      f'near/nudge updates kept/turned by native={turned}; carried flags after attach='
      f'{sorted(hex(v) for v in carried_flags)}; '
      f'position/velocity max raw deltas={worst[:6]}, BAM deltas={worst[6:]}; '
      'native collision callback admitted every substep and stopped correctly on code 2')
