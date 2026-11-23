#!/usr/bin/env python3
"""Compare VTOL_SeekAttack (4192f0), what an attack flyer does once its
target is lost, with retailVtolSeekAttack (src/sim/retailairseek.h), and the
lost-target exits that push it -- AirToGround's 41c8f6 and AirToAir's 41d654
-- with retailAirLostTarget.

The seek runs on random states of both stages and the head: the target offer
of stage 0 (4d7de0 with the carried target), the zero point, the orbit angle
and take-off (416c50); stage 1's slot call, target scan (4d8370) and attack
(4d7de0), the hop on a controller event, the orbit point at range + 160 and
its radius, the 30..59-tick sleep and the wait mask; the 0x200 end, the
return from off the map, the group flag's draw and every random draw (535cc0,
native, state at 0x64186c). Controller allocation, construction, the radius
setter and installation are host stubs that record their arguments; the
installation stub clears the pending controller events as 4d4d40 does. The
group check (51d1e0(unit, 1, 5), 4194f9) answers in or out of the slot at
random, on a mission flagged 0x8000000 or not, with the next mission (+0x66)
none, a group mover or not: the leave (50c480) is recorded and the
VTOL_Move_Formation push (row name 6054b8) counted. The air-traffic
counter (unit +0x12a) runs below, at and above 5: past stage 0 the seek
pushes VTOL_StepOut ahead (4d7750) with D = (rand(15) + footX) x 16.

The exits run 41bf80 and 41ca50 on a lost target (gone, dead, dying, carried,
off the map, or events 0x2/0x8/0x20) with the unit alive or not, dying or not,
the attack last in its list (+0x66) or not and every fire state: whether
VTOL_SEEKATTACK is pushed (4d77b0), the target it carries and its point
(mission +0x22).

    python3 tools/re/check_air_seek_attack.py [build-dbg/retail_motion_test]
"""
import random
import struct
import subprocess
import sys

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ECX

RNG = 0x64186c
SEEKATTACK = 0x6055a0
FORMATION = 0x6054b8


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-dbg/retail_motion_test'
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mission, kind, weapon, target, target_kind, mover, offmap, sector, controller, pushed = \
        [HEAP + i * 0x1000 for i in range(11)]
    found_unit = HEAP + 0x30000
    game = HEAP + 0x20000   # the game record reaches past +0x19f44

    def put(address, fmt, *values):
        p.uc.mem_write(address, struct.pack('<' + fmt, *values))

    def get(address, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(game + 0x19f30, 'I', offmap)
    log = {}
    answers = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        size = get(args)
        if size == 0x36:
            return 0, controller
        if size == 0x72:
            return 0, pushed
        raise AssertionError('unexpected allocation of %#x bytes' % size)

    def construct(uc, args):   # 4e40e0(mission, point), thiscall
        if get(args) != mission:
            raise AssertionError('controller for another mission')
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 4), 12)))
        put(controller + 0xa, 'h', 0)
        record('construct')
        return 2, controller

    def radius(uc, args):      # 4e4540(radius), thiscall
        put(controller + 0xa, 'H', get(args) & 0xffff)
        return 1, 0

    def install(uc, args):     # 4d4d40(controller), thiscall on the mission
        record('install', get(args))
        put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    def scan(uc, args):        # 4d8370(unit)
        if get(args) != unit:
            raise AssertionError('scan for another unit')
        record('scan')
        return 1, found_unit if answers['found'] else 0

    def attack(uc, args):      # 4d7de0(unit, target, explicit, ?, auto, ?)
        call = tuple(get(args + 4 * k) for k in range(6))
        record('attack', call)
        return 6, answers['retarget' if call[4] else 'offer']

    def lookup(uc, args):      # 4d4bf0(name), thiscall on a stack slot
        record('lookup', get(args))
        return 1, uc.reg_read(UC_X86_REG_ECX)

    def mission_ctor(uc, args):   # 4d6c40(row, target, point, +0x4e, +0x52, +0x56, vec, vec), thiscall
        if uc.reg_read(UC_X86_REG_ECX) != pushed:
            raise AssertionError('mission constructed elsewhere')
        point = bytes(uc.mem_read(get(args + 8), 12)) if get(args + 8) else b''
        record('mission', (get(args + 4), point, get(args + 12), get(args + 16), get(args + 20)))
        put(pushed + 4, 'B', 0x13)
        return 12, pushed

    def insert(uc, args):      # 4d77b0(unit, mission): at the end of the list
        record('insert', (get(args), get(args + 4)))
        return 2, 0

    def unexpected(name):
        def hook(uc, args):
            raise AssertionError('unexpected call to ' + name)
        return hook

    def group(uc, args):       # 51d1e0(unit, 1, 5)
        if (get(args), get(args + 4), get(args + 8)) != (unit, 1, 5):
            raise AssertionError('unexpected 51d1e0 arguments')
        record('group')
        return 3, 1 if answers.get('out') else 0

    p.hooks[0x51d1e0] = group
    p.hooks[0x50c480] = lambda uc, a: (record('leave', get(a)), (1, 0))[1]
    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = construct
    p.hooks[0x4e4540] = radius
    p.hooks[0x4d4d40] = install
    p.hooks[0x416c50] = lambda uc, a: (record('begin', tuple(get(a + 4 * k) for k in range(4))), (4, 0))[1]
    p.hooks[0x519b10] = lambda uc, a: (record('auto', get(a)), (1, 0))[1]
    p.hooks[0x519b70] = lambda uc, a: (record('manual', get(a)), (1, 0))[1]
    p.hooks[0x4d8370] = scan
    p.hooks[0x4d7de0] = attack
    p.hooks[0x4d4bf0] = lookup
    p.hooks[0x4d6c40] = mission_ctor
    p.hooks[0x4d77b0] = insert
    p.hooks[0x4d7750] = lambda uc, a: (record('push', (get(a), get(a + 4))), (2, 0))[1]
    for address, name in [(0x51a700, 'aim'), (0x51a770, 'ground aim'),
                          (0x4d6da0, 'mission destructor')]:
        p.hooks[address] = unexpected(name)
    p.freeze_hooks()
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1),
                  begin=0x535cc0, end=0x535cc0)

    # A flyer with a weapon (+0x130 0x8000000, alive), a mover, a canfly type
    # without weaponswitching, slot 0 not `dropped`; a target whose type
    # cannot fly.
    put(unit + 0x8, 'I', mover)
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xc, 'I', weapon)
    put(unit + 0x28, 'I', 0)
    put(unit + 0x44, 'I', 0)
    put(unit + 0x12a, 'B', 0)
    put(kind + 0x260, 'I', 0x800)
    put(kind + 0x1aa, 'I', weapon)
    put(weapon + 0xc8, 'I', 0)
    put(target + 0xb4, 'I', target_kind)
    put(target_kind + 0x260, 'I', 0)

    s32 = lambda v: (v + 2**31) % 2**32 - 2**31
    rng = random.Random(0x4192f0)
    following = HEAP + 0x32000     # the next mission, when +0x66 has one
    outcomes = {}

    # ---- VTOL_SeekAttack (4192f0) ----
    fixtures, expected = [], []
    for i in range(6000):
        stage = rng.choice((0, 0, 0, 1, 1, 1, 1, 1, 2))
        events = rng.choice((0, 0, 1, 1, 0x100, 0x400, 0x500, 0x200, 0x201, 0x501, 0x700, 0x2, 0x40, 0x4))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, 0xffff, rng.randrange(2**32)))
        flags = rng.choice((0x1000600, 0x1000600, 0x9000600, 0x8000000))
        angle = rng.choice((0, rng.randrange(65536), rng.randrange(2**32)))
        width = rng.choice((4096 - 32, 8192 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, 8192 - 128, rng.randrange(256, 12000)))
        x = rng.randrange(-600, width + 600) * 65536 + rng.randrange(65536)
        z = rng.randrange(-600, height + 600) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536 + rng.randrange(65536)
        spread = rng.choice((0, 16, 400, 2500))
        px = x + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        pz = z + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        py = rng.randrange(0, 256) * 65536
        if i % 7 == 0:
            px = py = pz = 0                      # stage 0's zero point
        elif i % 13 == 0:
            px, py, pz = 0, rng.randrange(1, 256) * 65536, 0
        weapon_range = rng.choice((2, 3, 140, 200, 450, 500, 510, rng.randrange(2, 2000)))
        armed = rng.random() < 0.95
        flies = rng.random() < 0.95
        carried = rng.random() < 0.3
        on_map = rng.random() < 0.7
        offer = int(rng.random() < 0.5)
        found = rng.random() < 0.3
        retarget = int(rng.random() < 0.5)
        seed = rng.randrange(2**32)
        traffic = rng.choice((0, 0, 0, 4, 5, 7, 17))
        foot = rng.choice((1, 2, 3, 4, 6))
        answers.update(found=found, offer=offer, retarget=retarget, out=rng.random() < 0.2)
        nxt = rng.choice((None, None, 0x3000400, 0x1000400, 0x5012000, 0x15000400, 0))

        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(unit + 0x130, 'I', 0x1000000 | (0x8000000 if armed else 0) | 0x80000)
        put(unit + 0x8, 'I', mover if flies else 0)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0xa4, 'I', sector if on_map else offmap)
        put(weapon + 0x90, 'i', weapon_range)
        put(unit + 0x12a, 'b', traffic)
        put(unit + 0x78, 'h', foot)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x16, 'I', target if carried else 0)
        put(mission + 0x22, 'iii', s32(px), s32(py), s32(pz))
        put(mission + 0x4e, 'I', angle)
        put(mission + 0x52, 'I', 0)
        put(mission + 0x5a, 'I', flags)
        put(mission + 0x66, 'I', following if nxt is not None else 0)
        put(following + 0x5a, 'I', nxt or 0)
        put(mission + 0x6a, 'I', pending)
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        result, error = p.call(0x4192f0, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        installs = len(log.get('install', []))
        if installs and log['install'][-1] != controller:
            raise AssertionError((i, 'installed a controller it did not build'))
        if any(a & 0xff != 3 for a in log.get('auto', [])) or any(a & 0xff != 3 for a in log.get('manual', [])):
            raise AssertionError((i, 'slot calls', log))
        offers = [c for c in log.get('attack', []) if c[4] == 0]
        attacks = [c for c in log.get('attack', []) if c[4] != 0]
        if any(c != (unit, target, 0, 0, 0, 0) for c in offers):
            raise AssertionError((i, 'stage 0 offer', offers))
        if any(c != (unit, found_unit, 0, 0, 1, 0) for c in attacks):
            raise AssertionError((i, 'stage 1 attack', attacks))
        if any(b != (unit, mission, 0, 0) for b in log.get('begin', [])):
            raise AssertionError((i, 'take-off', log['begin']))
        if log.get('begin') and get(mission + 0x52) != get(mission + 0x4e) & 1:
            raise AssertionError((i, '+0x52 is not the angle\'s low bit'))
        point = struct.unpack('<3i', p.uc.mem_read(controller + 0x26, 12)) if installs else (0, 0, 0)
        formations = [m for m, n in zip(log.get('mission', []), log.get('lookup', [])) if n == FORMATION]
        if any(m[4] != 0xfffffffc for m in formations):
            raise AssertionError((i, 'formation level', formations))
        if log.get('leave') and log['leave'] != [unit]:
            raise AssertionError((i, 'leave', log['leave']))
        if log.get('group'):
            key = ('formation' if formations else 'leave+50c480' if log.get('leave') else 'leave') \
                if answers['out'] else 'in slot'
            outcomes[key] = outcomes.get(key, 0) + 1
        expected.append((s32(result), get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa),
                         get(mission + 0x6a), get(mission + 0x5a), get(mission + 0x4e) & 0xffff,
                         *struct.unpack('<3i', p.uc.mem_read(mission + 0x22, 12)), installs,
                         *point, get(controller + 0xa, 'h') if installs else 0,
                         len(offers), len(log.get('begin', [])), len(log.get('auto', [])),
                         len(log.get('scan', [])), get(RNG), draws[0], len(log.get('push', [])) - len(formations),
                         s32(steps[-1][2]) if (steps := [m for m, n in zip(log.get('mission', []),
                                                                           log.get('lookup', []))
                                                         if n == 0x605288]) else 0,
                         len(log.get('group', [])), len(log.get('leave', [])), len(formations)))
        if any(c != (unit, pushed) for c in log.get('push', [])):
            raise AssertionError((i, 'push', log['push']))
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, angle & 0xffff,
                                           s32(px), s32(py), s32(pz), s32(x), s32(y), s32(z), weapon_range,
                                           int(armed), int(flies), int(carried), int(on_map), width, height,
                                           seed, offer, int(found and retarget), traffic, foot,
                                           int(answers['out']), nxt or 0))))
    output = subprocess.run([binary, '--air-seek'], input='\n'.join(fixtures) + '\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in output.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d, expected %d' % (len(actual), len(expected)))
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError('seek fixture %d: %s\n  port   %s\n  native %s' % (i, fixtures[i], got, want))
    stages = {}
    for fixture in fixtures:
        stages[int(fixture.split()[0])] = stages.get(int(fixture.split()[0]), 0) + 1
    installed = sum(1 for row in expected if row[10])
    hops = sum(1 for f, row in zip(fixtures, expected)
               if int(f.split()[0]) == 1 and row[0] == 2 and row[6] != int(f.split()[6]))
    print('PASS: %d VTOL_SeekAttack calls (stages %s), %d controllers, %d hops, %d step-outs, group check %s, '
          'every state, point, radius and draw'
          % (len(expected), ' '.join('%d:%d' % kv for kv in sorted(stages.items())), installed, hops,
             sum(1 for row in expected if row[21]), ', '.join('%s %d' % kv for kv in sorted(outcomes.items()))))

    # ---- the lost-target exits, 41c8f6, 41d654 and AirToGroundHover's 41e28f ----
    fixtures, expected = [], []
    hovers = 0
    for i in range(6000):
        hover = rng.random() < 0.33
        air_to_air = not hover and rng.random() < 0.5
        hovers += hover
        # AirToGroundHover's type has hoverattack (+0x260 0x8000000); its
        # exit, like AirToAir's, does not look at the unit's own state.
        put(kind + 0x260, 'I', 0x8000800 if hover else 0x800)
        alive = rng.random() < 0.85
        dying = rng.random() < 0.15
        last = rng.random() < 0.7
        fire = rng.choice((0, 0x40000, 0x80000, 0xc0000))
        state = rng.choice(('gone', 'dead', 'dying', 'carried', 'offmap', 'live'))
        events = rng.choice((0, 0x2, 0x8, 0x20, 0x28, 0x2a)) if not air_to_air and not hover \
            else rng.choice((0, 0x8, 0x20, 0x28))
        if hover and not events:
            events = rng.choice((0x8, 0x20, 0x28))   # its head reads a gone target's 0x200 path otherwise
        if state in ('gone', 'live') and not events:
            events = 0x8 if air_to_air else rng.choice((0x2, 0x8, 0x20))
        px, py, pz = (rng.randrange(-2**31, 2**31) for _ in range(3))
        put(unit + 0x130, 'I', 0x8000000 | (0x1000000 if alive else 0) | (0x1000 if dying else 0) | fire)
        put(unit + 0x8, 'I', mover)
        put(unit + 0xa4, 'I', sector)
        put(mission + 5, 'B', rng.randrange(8))
        put(mission + 6, 'I', 0)
        put(mission + 0x16, 'I', 0 if state == 'gone' else target)
        put(mission + 0x22, 'iii', px, py, pz)
        put(mission + 0x56, 'I', 0)
        put(mission + 0x5a, 'I', 0x1000200)
        put(mission + 0x66, 'I', 0 if last else pushed + 0x800)
        put(target + 0x130, 'I', {'dead': 0, 'dying': 0x1001001}.get(state, 0x1000001 if state != 'offmap' else 0x1000000))
        put(target + 0x114, 'B', 4 if state == 'carried' else 0)
        put(RNG, 'I', rng.randrange(2**32))
        answers['out'] = False
        log.clear()
        result, error = p.call(0x41d820 if hover else 0x41ca50 if air_to_air else 0x41bf80, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        if result != 5:
            raise AssertionError((i, 'the lost target did not end the attack', result))
        inserts = log.get('insert', [])
        built = log.get('mission', [])
        carried = 0
        if inserts:
            if inserts != [(unit, pushed)] or log.get('lookup') != [SEEKATTACK] or len(built) != 1:
                raise AssertionError((i, 'push', log))
            got_target, got_point, a, b, c = built[0]
            if got_point != struct.pack('<3i', px, py, pz) or (a, b, c) != (0, 0, 0):
                raise AssertionError((i, 'the seek does not carry mission +0x22', built))
            if got_target not in (0, target):
                raise AssertionError((i, 'carried target', built))
            carried = int(got_target == target)
        live = state in ('carried', 'offmap', 'live')
        expected.append((int(bool(inserts)), carried))
        fixtures.append(' '.join(map(str, (int(air_to_air or hover), int(alive), int(dying), int(last),
                                           int(fire == 0), int(state != 'gone'), int(live)))))
    output = subprocess.run([binary, '--air-lost'], input='\n'.join(fixtures) + '\n',
                            text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in output.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d, expected %d' % (len(actual), len(expected)))
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError('exit fixture %d: %s\n  port   %s\n  native %s' % (i, fixtures[i], got, want))
    pushes = sum(row[0] for row in expected)
    carries = sum(row[1] for row in expected)
    put(kind + 0x260, 'I', 0x800)
    print('PASS: %d lost-target exits (AirToGround, AirToAir and %d AirToGroundHover), %d pushes, %d carrying the '
          'target, every gate' % (len(expected), hovers, pushes, carries))


if __name__ == '__main__':
    main()
