#!/usr/bin/env python3
"""Observe the ordinary LOS update timeline in the user's retail executable.

Collision, sound, trail, damage and destruction are controlled sinks. This
establishes update sequencing, not collision geometry or damage correctness.
"""
import itertools
import struct
import sys
from emu import Icd, HEAP

p = Icd()
game, weapon, shot, owner, emitter = [HEAP + i * 0x10000 for i in range(5)]
events = []
now = calls = collision_at = 0
collision_code = 0
trail_alive = False
lightning = '--lightning' in sys.argv[1:]


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def collision(uc, sp):
    global calls
    assert read(sp) == shot
    calls += 1
    code = collision_code if calls == collision_at else 0
    if code == 2:
        put(shot + 0x80, 123)
    return 1, code


def impact(uc, sp):
    assert struct.unpack('<5I', uc.mem_read(sp, 20)) == (shot, 123, 0, 1, 0)
    events.append(('impact', now))
    return 5, 0


def destroy(uc, sp):
    assert read(sp) == shot
    events.append(('destroy', now))
    return 1, 0


def trail(uc, sp):
    events.append(('trail', read(sp)))
    return 1, int(trail_alive)


p.hooks.update({0x52a4d0: collision, 0x529c10: impact, 0x529af0: destroy,
                0x530730: lambda uc, sp: (1, 0), 0x4f46b0: trail})
p.freeze_hooks()
put(0x62d55c, game)
count = 0
for steps, collision_code, death_tick, dead_flags, has_trail in itertools.product(
        (1, 2, 4), (0, 1, 2), (98, 100, 101, 1000), (0, 0x1001000), (False, True)):
    for collision_at in (1, 3, 9):
        p.uc.mem_write(shot, bytes(0xb0))
        put(shot, weapon)
        put(shot + 0x7c, owner)
        put(shot + 0x60, 100)
        put(shot + 0x70, 106)
        put(shot + 0xa4, emitter if has_trail else 0)
        put(weapon + 0xd0, steps)
        p.uc.mem_write(weapon + 0xb8, struct.pack('<3H', 65530, 17, 33))
        p.uc.mem_write(owner + 0x7e, struct.pack('<H', 1234))
        velocity = (0x70000001, 0xfffffffd, 0x12345678)
        p.uc.mem_write(shot + 0x1c, struct.pack('<3I', *velocity))
        calls = 0
        angles = [0, 0, 0]
        position = [0, 0, 0]
        impacted = False
        for now in range(98, 111):
            events.clear()
            put(game + 0x19f44, now)
            put(owner + 0x130, dead_flags if now >= death_tick else 0x1000000)
            trail_alive = now < 108
            expected = []
            old_calls = calls
            if lightning and now >= death_tick:
                expected.append(('destroy', now))
            elif now <= 100:
                if now >= death_tick:
                    expected.append(('destroy', now))
                elif now == 100:
                    angles[1] = (angles[1] + 1234) & 65535
            elif not impacted and now < 106:
                if has_trail:
                    expected.append(('trail', 1))
                for step in range(steps):
                    position = [(x + v) & 0xffffffff for x, v in zip(position, velocity)]
                    angles = [(x + v) & 65535 for x, v in zip(angles, (65530, 17, 33))]
                    if collision_code == 2 and old_calls + step + 1 == collision_at:
                        expected.append(('impact', now))
                        impacted = True
                        break
            else:
                if has_trail:
                    expected.append(('trail', 0))
                if not has_trail or not trail_alive:
                    expected.append(('destroy', now))
            _, error = p.call(0x52ccc0 if lightning else 0x52c6d0, (shot,), ecx=weapon)
            assert not error, error
            context = (steps, collision_code, death_tick, dead_flags, has_trail, collision_at, now)
            assert events == expected, (context, events, expected)
            assert struct.unpack('<3I', p.uc.mem_read(shot + 4, 12)) == tuple(position), context
            assert struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6)) == tuple(angles), context
            assert bool(p.uc.mem_read(shot + 0x97, 1)[0]) == impacted, context
            if ('destroy', now) in events:
                break
        count += 1
print(f'PASS: {count} native {"lightning" if lightning else "straight-shot"} timelines: delayed motion, wrapped position/rotation, '
      'outside-map continuation, single impact, exclusive expiry, owner-death timing and trail draining')
