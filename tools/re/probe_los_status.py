#!/usr/bin/env python3
"""Observe the three Line-of-Sight status classes in the user's retail executable.

LineOfSightMindControl, LineOfSightTurnToStone and LineOfSightTurnToFrozen
launch (52c540) and draw (52c8c0) like an ordinary straight shot but have
their own update and impact callback:

    class          update   callback
    MindControl    52d8d0   52da10  (convert, 514da0)
    TurnToStone    52db40   52dca0  (statue, 51a140 type 14)
    TurnToFrozen   52dda0   52df00  (statue, 51a140 type 15)

Part 1 runs each update over controlled timelines (collision, impact and
destruction are sinks) and compares every call against the model World
ports: the owner check, activation without the body heading, flight on the
expiry tick, and an impact that hands the struck unit to 529c10 with the
class's callback and retires the shot.

Part 2 runs each callback against a controlled unit and CRT rand() and
compares the gates, the single draw and the roll (rand()*100/32768 against
the truncated chance*scale) with the model, with the caster's player at its
unit cap or not: the charm calls 514da0 either way (World's captureUnit
leaves the refusal to changeOwner, after the capture event).

    python3 tools/re/probe_los_status.py
"""
import itertools
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

UPDATES = {'mind': (0x52D8D0, 0x52DA10), 'stone': (0x52DB40, 0x52DCA0), 'frozen': (0x52DDA0, 0x52DF00)}

p = Icd()
game, weapon, shot, owner, unit, unitdef, mover = [HEAP + i * 0x10000 for i in range(7)]
events = []
now = calls = collision_at = 0
collision_code = 0
rand_value = 0
level = 0


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
    events.append(('impact', now, struct.unpack('<5I', uc.mem_read(sp, 20))))
    return 5, 0


def destroy(uc, sp):
    assert read(sp) == shot
    events.append(('destroy', now))
    return 1, 0


def get_level(uc, sp):
    assert read(sp) == unit
    return 1, level


def crt_rand(uc, sp):
    events.append(('rand',))
    return 0, rand_value


def convert(uc, sp):
    events.append(('convert',) + struct.unpack('<3I', uc.mem_read(sp, 12)))
    return 3, 0


def damage(uc, sp):
    events.append(('damage',) + struct.unpack('<5I', uc.mem_read(sp, 20)))
    return 5, 0


def explosion(uc, sp):
    events.append(('explosion',) + struct.unpack('<3I', uc.mem_read(sp, 12)))
    return 3, 0


p.hooks.update({0x52A4D0: collision, 0x529C10: impact, 0x529AF0: destroy,
                0x519310: get_level, 0x5D4444: crt_rand, 0x514DA0: convert,
                0x51A140: damage, 0x492C80: explosion})
p.freeze_hooks()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
put(0x62D55C, game)

# ---- Part 1: update timelines -------------------------------------------------
timelines = 0
for name, (update, callback) in UPDATES.items():
    for steps, collision_code, death_tick, dead_flags in itertools.product(
            (1, 2, 4), (0, 1, 2), (98, 100, 101, 104, 1000), (0, 0x1001000)):
        for collision_at in (1, 3, 9, 13):
            p.uc.mem_write(shot, bytes(0xb0))
            put(shot, weapon)
            put(shot + 0x7c, owner)
            put(shot + 0x60, 100)        # start (activation) tick
            put(shot + 0x70, 106)        # expiry tick
            put(weapon + 0xd0, steps)
            put(weapon + 0x54, 0)        # no foreground/shadow animation handles
            put(weapon + 0x58, 0)
            p.uc.mem_write(weapon + 0xb8, struct.pack('<3H', 65530, 17, 33))
            p.uc.mem_write(owner + 0x7e, struct.pack('<H', 1234))
            velocity = (0x70000001, 0xfffffffd, 0x12345678)
            p.uc.mem_write(shot + 0x1c, struct.pack('<3I', *velocity))
            calls = 0
            angles = [0, 0, 0]
            position = [0, 0, 0]
            for now in range(98, 111):
                events.clear()
                put(game + 0x19F44, now)
                put(owner + 0x130, dead_flags if now >= death_tick else 0x1000000)
                dead = now >= death_tick and dead_flags != 0x1000000
                expected = []
                old_calls = calls
                # The model World ports (tickStraightProjectiles, status branch).
                if name == 'mind' and dead:
                    expected.append(('destroy', now))
                elif name != 'mind' and dead and now <= 100:
                    expected.append(('destroy', now))
                if now > 100:
                    hit = False
                    for step in range(steps):
                        position = [(x + v) & 0xffffffff for x, v in zip(position, velocity)]
                        angles = [(x + v) & 65535 for x, v in zip(angles, (65530, 17, 33))]
                        if collision_code == 2 and old_calls + step + 1 == collision_at:
                            expected.append(('impact', now, (shot, 123, 1, 0, callback)))
                            hit = True
                            break
                    if not hit and now >= 106:
                        expected.append(('destroy', now))
                _, error = p.call(update, (shot,), ecx=weapon)
                assert not error, error
                context = (name, steps, collision_code, death_tick, dead_flags, collision_at, now)
                assert events == expected, (context, events, expected)
                assert struct.unpack('<3I', p.uc.mem_read(shot + 4, 12)) == tuple(position), context
                assert struct.unpack('<3H', p.uc.mem_read(shot + 0x34, 6)) == tuple(angles), context
                # The manager compacts a destroyed shot at the end of the tick, and
                # 529c10's destroy flag retires one that struck: no later update.
                if any(e[0] in ('destroy', 'impact') for e in events):
                    break
            timelines += 1

# ---- Part 2: impact callbacks ------------------------------------------------
def model(kind, same, has_mover, commander, immune, remaining, transport, lvl, r, scale):
    """The gates and roll World ports (World::applyStatusImpact)."""
    if same or not has_mover or commander:
        return 0, []
    if kind == 'mind':
        if remaining > 0 or transport or immune:
            return 0, []
    else:
        if immune or remaining > 0:
            return 0, []
    chance = min(99, (lvl + 16) * 5)
    roll = (r * 100) // 32768
    threshold = int(chance * struct.unpack('<f', struct.pack('<f', scale))[0])
    if roll >= threshold:
        return 1, [('rand',)]
    effect = ('convert', unit, game + 0x2404 + 3 * 0x110, 0) if kind == 'mind' else \
        ('damage', owner, unit, 1, 14 if kind == 'stone' else 15, 0)
    return 2, [('rand',), effect, ('explosion', unit + 0x68, 0x4444, 0x5555)]


callbacks = 0
for name, (_, callback) in UPDATES.items():
    immune_word, immune_bit = {'mind': (0x264, 0x100000), 'stone': (0x268, 2), 'frozen': (0x268, 4)}[name]
    for same, has_mover, commander, immune, remaining, transport, at_cap in itertools.product(
            (False, True), (False, True), (False, True), (False, True), (0.0, 0.25), (False, True),
            (False, True)):
        for level, rand_value, scale in itertools.product(
                (0, 1, 3, 4, 10), (0, 327, 26213, 26214, 26215, 32439, 32767), (1.0, 0.75, 0.33)):
            p.uc.mem_write(unit, bytes(0x140))
            p.uc.mem_write(unitdef, bytes(0x2a0))
            p.uc.mem_write(shot, bytes(0xb0))
            put(shot, weapon)
            put(shot + 0x7c, owner)
            p.uc.mem_write(shot + 0x92, bytes([3]))
            put(weapon + 0x6c, 0x4444)
            put(weapon + 0x74, 0x5555)
            p.uc.mem_write(unit + 0xfd, bytes([3 if same else 1]))
            put(unit + 8, mover if has_mover else 0)
            put(unit + 0xb4, unitdef)
            put(unitdef + 0x264, (0x40000 if commander else 0) | (immune_bit if immune and immune_word == 0x264 else 0))
            put(unitdef + 0x268, immune_bit if immune and immune_word == 0x268 else 0)
            p.uc.mem_write(unit + 0x108, struct.pack('<f', remaining))
            put(unit + 0xa8, 0x77 if transport else 0)
            # The caster's player (3) at its unit cap or not: the game's cap
            # (+0x14e7c) against the owner record's count (+0xe8), what
            # 512060 refuses on (check_change_owner.py). The callbacks read
            # neither: the charm calls 514da0 either way.
            put(game + 0x14e7c, 250)
            put(game + 0x2404 + 3 * 0x110 + 0xe8, 250 if at_cap else 10)
            events.clear()
            result, error = p.call(callback, (shot, unit, struct.unpack('<I', struct.pack('<f', scale))[0]))
            assert not error, error
            want, want_events = model(name, same, has_mover, commander, immune, remaining, transport,
                                      level, rand_value, scale)
            context = (name, same, has_mover, commander, immune, remaining, transport, at_cap, level, rand_value,
                       scale)
            assert result == want, (context, result, want)
            assert events == want_events, (context, events, want_events)
            callbacks += 1

print(f'PASS: {timelines} native status-shot timelines (owner check, no activation heading, flight '
      f'on the expiry tick, impact with the class callback) and {callbacks} callback cases '
      f'(gates, one CRT draw, rand()*100/32768 < (int)(chance*scale), effect and explosion on success, '
      f'the charm\'s 514da0 at the new owner\'s unit cap too)')
