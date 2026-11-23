#!/usr/bin/env python3
"""Compare AirToAir (41ca50), the attack mission of a flyer whose target can
fly, and VTOL_Evade (41e3d0), the break-off it pushes, with retailAirToAir and
retailAirEvade (src/sim/retailairtoair.h). Also the facing test both use
(41d780) and the moving controller they install (vtable 5f29a8: 4e48d0,
4e4b70, 4e4be0).

The handlers run natively on random states of every stage: the pursuit and
reposition points, the evade accumulator at mission +0x4e, the break-off, the
retarget scan of an auto attack, the return from off the map, the 0x28/0x40
events and every random draw (535cc0, native, state at 0x64186c). Native too:
41d780, 51b340, 53612a, 5360bf/5360f3, 536143, 421ae0, 519ac0, 4d6a10 and the
x87 helpers. Host stubs record their arguments: the led SweetSpot 51aa50
(answers a controlled point), the slot calls, BeginFlight 416c50, the target
scan 4d8370 and attack 4d7de0 (controlled answers), controller allocation and
construction (4e40e0 + radius 4e4540, 4e4560; 4e4c60 runs natively on the
returned buffer), installation 4d4d40 (clears the pending controller events
as it does), and the mission push (4d4bf0 + 4d6c40 + 4d7750). The group check
(51d1e0(unit, 1, 5), 41ccde in AirToAir and 41e55a in VTOL_Evade) answers in
or out of the slot at random, on a mission flagged 0x8000000 or not, with the
next mission (+0x66) none, a group mover or not: the leave (50c480) is
recorded and the VTOL_Move_Formation push counted by its row name, built or
not as the fixture says. The air-traffic counter (unit
+0x12a) runs below, at and above 5: stage 2 then rescans an auto attack and
pushes VTOL_StepOut with D = (rand(15) + the target's footX + its own) x 16.
A lost target's VTOL_SEEKATTACK push (41d654) is counted here;
check_air_seek_attack.py compares it.

    python3 tools/re/check_air_to_air.py [build-o2/retail_motion_test]
"""
import math
import random
import struct
import subprocess
import sys

import emurange
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

RNG = 0x64186c


def s32(v):
    return (v + 2**31) % 2**32 - 2**31


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_motion_test'
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, kind, mover, weapon, target, target_kind, target_mover, mission, offmap, sector, point_goal, \
        moving_goal, pushed, owner = [HEAP + i * 0x1000 for i in range(14)]
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
            return 0, point_goal
        if size == 0x2c:
            p.uc.mem_write(moving_goal, bytes(0x2c))
            return 0, moving_goal
        if size == 0x72:
            return 0, pushed
        raise AssertionError('unexpected allocation of %#x bytes' % size)

    def point_ctor(uc, args):      # 4e40e0(mission, &point), thiscall
        record('point', struct.unpack('<3i', uc.mem_read(get(args + 4), 12)))
        put(point_goal + 0xa, 'h', 0)
        return 2, point_goal

    def radius(uc, args):          # 4e4540(radius), thiscall
        record('radius', s32(get(args)) & 0xffff)
        return 1, 0

    def moving_ctor(uc, args):     # 4e4560(mission, &point, &velocity), thiscall
        point = struct.unpack('<3i', uc.mem_read(get(args + 4), 12))
        velocity = struct.unpack('<3i', uc.mem_read(get(args + 8), 12))
        record('moving', (point, velocity))
        return 3, moving_goal

    def install(uc, args):         # 4d4d40(controller), thiscall on the mission
        controller = get(args)
        record('install', controller)
        if controller:
            put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    def descriptor(uc, args):      # 4d4bf0(name), thiscall
        name = bytes(uc.mem_read(get(args), 32)).split(b'\0')[0].decode()
        record('descriptor', name)
        return 1, 0

    def build(uc, args):           # 4d6c40(row, target, point, +0x4e, ...), thiscall, 12 dwords
        record('built', (log['descriptor'][-1], get(args + 12, 'i')))
        put(pushed + 4, 'B', answers['inserted'])
        return 12, pushed

    def scan(uc, args):            # 4d8370(unit)
        record('scan')
        return 1, answers['scan']

    def attack(uc, args):          # 4d7de0(unit, target, 0, 0, 1, 0)
        values = struct.unpack('<6I', uc.mem_read(args, 24))
        if values != (unit, answers['scan'], 0, 0, 1, 0):
            raise AssertionError(('attack', values))
        record('attack')
        return 6, answers['attack']

    def lead(uc, args):            # 51aa50(unit, &out, slot)
        if get(args) != unit or get(args + 8) & 0xff != 0:
            raise AssertionError('lead for another unit or slot')
        p.uc.mem_write(get(args + 4), struct.pack('<3i', *answers['lead']))
        return 3, 1

    def unexpected(name):
        def hook(uc, args):
            raise AssertionError('unexpected call to ' + name)
        return hook

    def group(uc, args):           # 51d1e0(unit, 1, 5)
        if (get(args), get(args + 4), get(args + 8)) != (unit, 1, 5):
            raise AssertionError('unexpected 51d1e0 arguments')
        record('group')
        return 3, 1 if answers.get('out') else 0

    p.hooks[0x51d1e0] = group
    p.hooks[0x50c480] = lambda uc, a: (record('leave', get(a)), (1, 0))[1]
    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = point_ctor
    p.hooks[0x4e4540] = radius
    p.hooks[0x4e4560] = moving_ctor
    p.hooks[0x4d4d40] = install
    p.hooks[0x4d4bf0] = descriptor
    p.hooks[0x4d6c40] = build
    p.hooks[0x4d7750] = lambda uc, a: (record('push', get(a + 4)), (2, 0))[1]
    p.hooks[0x4d77b0] = lambda uc, a: (record('insert', get(a + 4)), (2, 0))[1]
    p.hooks[0x4d6da0] = lambda uc, a: (record('destroy'), (0, 0))[1]
    p.hooks[0x4eba00] = lambda uc, a: (0, 0)
    p.hooks[0x4d8370] = scan
    p.hooks[0x4d7de0] = attack
    p.hooks[0x51aa50] = lead
    p.hooks[0x416c50] = lambda uc, a: (record('begin', struct.unpack('<4I', uc.mem_read(a, 16))), (4, 0))[1]
    p.hooks[0x519b10] = lambda uc, a: (record('auto', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x519b70] = lambda uc, a: (record('manual', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x51a700] = lambda uc, a: (record('aim', (get(a), get(a + 4), get(a + 8) & 0xff)), (3, 0))[1]
    for address, name in [(0x51a770, 'ground aim')]:
        p.hooks[address] = unexpected(name)
    p.freeze_hooks()
    # Watch (emurange.py) sees retail leave the 16.16 range in a call: a sum of positions wraps in AirToAir, the
    # facing test or VTOL_Evade, or their distance to the target (41d0b4, 41e6d0) is 32768 px or more and reads
    # negative. VTOL_Evade's line (41e80d..41e9a6) wraps in the port too. LAST are the sums of the point AirToAir
    # installs, the last thing it builds: nothing is derived from them.
    watch = emurange.Watch(p, ((0x41ca50, 0x41d81f), (0x41e3d0, 0x41eeff)) + emurange.HELPERS,
                           words=(0x41d0b4, 0x41e6d0), kept=(0x41e80d, 0x41e818, 0x41e9a1, 0x41e9a6))
    LAST = frozenset((0x41d3f6, 0x41d3fd, 0x41d53c, 0x41d53e))
    draws = [0]
    bounds = []

    def draw(uc, address, size, data):
        draws[0] += 1
        bounds.append(get(uc.reg_read(UC_X86_REG_ESP) + 4))
    p.uc.hook_add(UC_HOOK_CODE, draw, begin=0x535cc0, end=0x535cc0)

    rng = random.Random(0x41ca50)
    following = HEAP + 0x32000     # the next mission, when +0x66 has one
    group_stats = {}

    def group_case(nxt):
        put(mission + 0x66, 'I', following if nxt is not None else 0)
        put(following + 0x5a, 'I', nxt or 0)

    def group_outcome(result, formations):
        if not log.get('group'):
            return
        if log.get('leave') and log['leave'] != [unit]:
            raise AssertionError(('leave', log['leave']))
        key = ('formation' if formations else 'leave+50c480' if log.get('leave') else 'leave') \
            if answers['out'] else 'in slot'
        if answers['out'] and not formations and result != 8:
            raise AssertionError(('out of slot without leave or formation', result))
        group_stats[key] = group_stats.get(key, 0) + 1

    def run(port_mode, rows):
        output = subprocess.run([binary, port_mode], input='\n'.join(rows) + '\n',
                                text=True, capture_output=True, check=True)
        return [tuple(map(int, line.split())) for line in output.stdout.splitlines()]

    # `left`, per fixture: where retail left the range in that call (watch.left()). Only such a fixture may
    # differ: the port stops at the end of the range and reads the distance whole (ours). One that left it only
    # in the sums of the installed point may differ in that point's columns alone (`point`); from any other
    # sum or the distance follow headings, tests and draws. Every other fixture must match exactly.
    def compare(name, fixtures, expected, actual, left=None, point=()):
        if len(actual) != len(expected):
            raise AssertionError('%s: row count %d, expected %d' % (name, len(actual), len(expected)))
        differ = alone = 0
        for i, (got, want) in enumerate(zip(actual, expected)):
            if got != want:
                at_point = emurange.stopped(got, want, point)
                if left and left[i] and (at_point or not left[i] <= LAST):
                    differ += 1
                    alone += at_point
                    continue
                raise AssertionError('%s fixture %d: %s\n  port   %s\n  native %s' % (name, i, fixtures[i], got, want))
        if left:
            print('%s: retail leaves the range in %d fixtures (a sum wraps, or 32768 px reads negative); the port '
                  'differs in %d of them, %d in the installed point alone (ours)'
                  % (name, sum(map(bool, left)), differ, alone))

    # --- 41d780 ----------------------------------------------------------
    fixtures, expected = [], []
    a_unit, b_unit = unit, target
    for i in range(20000):
        ax, az = rng.randrange(-2**28, 2**28), rng.randrange(-2**28, 2**28)
        spread = rng.choice((0, 1, 65536, 20 * 65536, 300 * 65536, 2**28))
        bx, bz = ax + rng.randrange(-spread, spread + 1), az + rng.randrange(-spread, spread + 1)
        heading, octant = rng.randrange(65536), rng.randrange(8)
        put(a_unit + 0x68, 'iii', ax, 0, az)
        put(b_unit + 0x68, 'iii', bx, 0, bz)
        put(b_unit + 0x7e, 'H', heading)
        result, error = p.call(0x41d780, (a_unit, b_unit, octant))
        if error:
            raise AssertionError((i, error))
        expected.append((result & 0xff,))
        fixtures.append('%d %d %d %d %d %d' % (ax, az, bx, bz, heading, octant))
    compare('41d780', fixtures, expected, run('--air-ahead', fixtures))
    print('41d780: %d facing tests match (%d ahead)' % (len(fixtures), sum(r[0] for r in expected)))

    # --- the moving controller ------------------------------------------
    fixtures, expected = [], []
    controller = moving_goal
    put(owner + 8, 'I', mover)
    put(owner + 0xb4, 'I', kind)
    put(mover + 0x36, 'H', 0)
    for i in range(20000):
        point = [rng.randrange(-2**27, 2**27) for _ in range(3)]
        velocity = [rng.choice((0, rng.randrange(-8 * 65536, 8 * 65536), rng.randrange(-65536, 65536)))
                    for _ in range(3)]
        if i % 9 == 0:
            velocity = [0, 0, 0]
        heading, turns = rng.randrange(65536), rng.randrange(2)
        turn_rate = rng.choice((0, 7, 8, 100, 115, 200, 500, 700, 1000, rng.randrange(65536)))
        near = rng.choice((0, 30, 47, 48, 49, 200, 400))
        ox = point[0] + velocity[0] + rng.randrange(-near, near + 1) * 65536 + rng.randrange(65536)
        oz = point[2] + velocity[2] + rng.randrange(-near, near + 1) * 65536 + rng.randrange(65536)
        if i % 13 == 0:   # exactly 48 px out along x, after the move
            ox, oz = point[0] + velocity[0] + 48 * 65536 + rng.choice((-1, 0, 1)), point[2] + velocity[2]
        body = rng.randrange(65536)
        p.uc.mem_write(controller, bytes(0x2c))
        put(controller, 'I', 0x5f29a8)
        put(controller + 8, 'H', turns)
        put(controller + 0xa, 'iii', *point)
        put(controller + 0x16, 'iii', *velocity)
        put(controller + 0x24, 'H', heading)
        put(controller + 0x28, 'I', owner)
        put(kind + 0x18e, 'H', turn_rate)
        put(owner + 0x68, 'iii', ox, 0, oz)
        put(owner + 0x7e, 'H', body)
        out = HEAP + 0x30000
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = p.call(0x4e48d0, (out,), ecx=controller)
        if error or result != 1:
            raise AssertionError((i, error, result))
        answer = struct.unpack('<3i', p.uc.mem_read(out, 12))
        moved = struct.unpack('<3i', p.uc.mem_read(controller + 0xa, 12))
        turned = struct.unpack('<3i', p.uc.mem_read(controller + 0x16, 12))
        accept, error = p.call(0x4e4be0, (owner,), ecx=controller)
        if error:
            raise AssertionError((i, error))
        result, error = p.call(0x4e4b70, (out,), ecx=controller)
        if error or result != 1:
            raise AssertionError((i, error, result))
        expected.append((*answer, moved[0], moved[2], *turned, accept & 0xff, get(out, 'H')))
        fixtures.append(' '.join(map(str, (*point, *velocity, heading, turns, turn_rate, ox, oz, body))))
    compare('5f29a8', fixtures, expected, run('--air-intercept', fixtures))
    print('5f29a8: %d navigator calls match (%d within 48 px, %d turned)' %
          (len(fixtures), sum(r[8] for r in expected),
           sum(1 for f, r in zip(fixtures, expected) if tuple(map(int, f.split()))[3:6] != r[5:8])))

    # --- AirToAir 41ca50 -------------------------------------------------
    put(unit + 0x8, 'I', mover)
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xc, 'I', weapon)
    put(unit + 0x28, 'I', 0)
    put(unit + 0x44, 'I', 0)
    put(unit + 0x12a, 'B', 0)
    put(target + 0x8, 'I', target_mover)
    put(target + 0x114, 'B', 0)
    put(target + 0xb4, 'I', target_kind)
    put(target_kind + 0x260, 'I', 0x800)
    fixtures, expected, left = [], [], []
    seeks = 0
    watch.left()
    stats = {}
    for i in range(12000):
        stage = rng.choice((0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3))
        events = rng.choice((0, 0, 0, 1, 1, 1, 0x100, 0x400, 0x8, 0x20, 0x40, 0x41, 0x28, 0x700))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, 0xffff, rng.randrange(2**32)))
        flags = rng.choice((0, 0, 0x1000200, 0x8000000, 0x9000200))
        count = rng.choice((0, 0, 7, 21, 40, 60, 90, 118, 119, 200, rng.randrange(-50, 300)))
        width = rng.choice((4096 - 32, 8192 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, 8192 - 128, rng.randrange(256, 12000)))
        x = rng.randrange(0, width) * 65536 + rng.randrange(65536)
        z = rng.randrange(0, height) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536
        weapon_reach = rng.choice((0, 0, 0, 1, 50, 150, rng.randrange(65536)))
        unit_reach = rng.choice((100, 100, 212, 0, 1, 2, 3, 7, 141, 250, rng.randrange(1000), rng.randrange(65536)))
        reach = weapon_reach or unit_reach
        spread = rng.choice((0, 16, reach // 4 + 1, reach // 2 + 1, reach + 1, 2 * reach + 1, 6 * reach + 40, 3000))
        tx = x + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        tz = z + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        ty = rng.randrange(0, 512) * 65536
        lead = (s32(tx + rng.randrange(-40, 41) * 65536), s32(ty + rng.randrange(0, 40) * 65536),
                s32(tz + rng.randrange(-40, 41) * 65536))
        if i % 17 == 0:
            lead = (s32(x), s32(ty), s32(z))
        heading, target_heading = rng.randrange(65536), rng.randrange(65536)
        if i % 5 == 0:   # the target straight ahead of or behind the flyer
            angle = rng.choice((heading, (heading + 0x8000) & 0xffff))
            d = rng.randrange(1, 400)
            tx = x - int(d * 65536 * math.sin(angle * math.pi / 32768))
            tz = z - int(d * 65536 * math.cos(angle * math.pi / 32768))
            target_heading = (rng.choice((heading, heading + 0x8000, heading + 0x4000)) + rng.randrange(-0x1000, 0x1000)) & 0xffff
        velocity = tuple(rng.randrange(-6 * 65536, 6 * 65536) for _ in range(3))
        max_speed = rng.choice((rng.randrange(65536, 6 * 65536), 3 * 65536, 0))
        speed = rng.choice((0, max_speed // 2, max_speed // 2 + 1, max_speed, rng.randrange(0, 6 * 65536)))
        automatic = rng.randrange(2)
        flies = 0 if i % 41 == 0 else 1
        on_map = rng.random() < 0.94
        seed = rng.randrange(2**32)
        found = rng.choice((0, target, unit + 0x800))
        answers.update(scan=found, attack=rng.choice((0, 1)), inserted=rng.choice((0, 1, 1, 1)), lead=lead,
                       out=rng.random() < 0.2)
        nxt = rng.choice((None, None, 0x3000400, 0x1000400, 0x5012000, 0x15000400, 0))
        retarget = int(found not in (0, target) and answers['attack'] != 0)
        fire_bits = rng.choice((0, 0, 0x40000, 0xc0000))
        traffic = rng.choice((0, 0, 0, 4, 5, 6, 17))
        foot, target_foot = rng.choice((1, 2, 3, 4, 6)), rng.choice((1, 2, 3, 5))

        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(unit + 0x130, 'I', 0x9000000 | fire_bits)
        put(unit + 0x12a, 'b', traffic)
        put(unit + 0x78, 'h', foot)
        put(target + 0x78, 'h', target_foot)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0x7e, 'H', heading)
        put(unit + 0xa4, 'I', sector if on_map else offmap)
        p.uc.mem_write(unit + 0x12b, struct.pack('<i', max_speed))
        put(mover + 0x20, 'i', speed)
        put(mover + 0x36, 'H', 0)
        put(kind + 0x260, 'I', 0x800 if flies else 0)
        put(kind + 0x25a, 'H', unit_reach)
        put(weapon + 0xc6, 'H', weapon_reach)
        put(target + 0x130, 'I', rng.choice((0x1000001, 0x1000002)))
        put(target + 0x68, 'iii', s32(tx), s32(ty), s32(tz))
        put(target + 0x7e, 'H', target_heading)
        put(target_mover + 8, 'iii', *velocity)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x16, 'I', target)
        put(mission + 0x4e, 'i', count)
        put(mission + 0x52, 'I', automatic)
        put(mission + 0x56, 'I', 0)
        put(mission + 0x5a, 'I', flags)
        group_case(nxt)
        put(mission + 0x6a, 'I', pending)
        put(point_goal + 0xa, 'h', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        bounds.clear()
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = p.call(0x41ca50, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        left.append(watch.left())
        names = log.get('descriptor', [])
        seek = names.count('VTOL_SEEKATTACK')
        seeks += seek
        evades = names.count('VTOL_EVADE')
        formations = names.count('VTOL_Move_Formation')
        group_outcome(s32(result), formations)
        steps = [b[1] for b in log.get('built', []) if b[0] == 'VTOL_StepOut']
        if set(names) - {'VTOL_SEEKATTACK', 'VTOL_EVADE', 'VTOL_StepOut', 'VTOL_Move_Formation'}:
            raise AssertionError((i, 'pushed', names))
        installs = [c for c in log.get('install', []) if c]
        releases = log.get('install', []).count(0)
        if log.get('begin') and log['begin'][0] != (unit, mission, 0, 0):
            raise AssertionError((i, 'begin', log['begin']))
        if any(s != 3 for s in log.get('auto', [])) or any(s != 0 for s in log.get('manual', [])) or \
                any(a != (unit, target, 0) for a in log.get('aim', [])):
            raise AssertionError((i, 'slot calls', log))
        points, moving = log.get('point', []), log.get('moving', [])
        if len(installs) != len(points) + len(moving):
            raise AssertionError((i, 'installed a controller it did not build', log))
        goal = points[-1] if points else (0, 0, 0)
        goal_radius = log['radius'][-1] if points else 0
        if moving:
            (mx, my, mz), (vx, vy, vz) = moving[-1]
            mh = get(moving_goal + 0x24, 'H')
            if not get(moving_goal + 8, 'H') & 1:
                raise AssertionError((i, 'moving controller without a heading'))
        else:
            mx = my = mz = vx = vy = vz = mh = 0
        if log.get('push') and log['push'][-1] != pushed or log.get('insert') and log['insert'][-1] != pushed:
            raise AssertionError((i, 'pushed another mission'))
        expected.append((s32(result), get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa),
                         get(mission + 0x6a), get(mission + 0x5a), get(mission + 0x4e, 'i'), len(points),
                         len(moving), *goal, goal_radius, mx, my, mz, vx, vy, vz, mh,
                         len(log.get('begin', [])), len(log.get('auto', [])), len(log.get('scan', [])),
                         releases, evades, get(RNG), draws[0], len(steps), steps[-1] if steps else 0,
                         len(log.get('group', [])), len(log.get('leave', [])), formations))
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, count, s32(x), s32(y),
                                           s32(z), heading, s32(tx), s32(ty), s32(tz), target_heading, *velocity,
                                           *lead, speed, max_speed, reach, automatic, flies, int(on_map), width,
                                           height, seed, retarget, answers['inserted'], traffic, foot,
                                           target_foot, int(answers['out']), nxt or 0))))
        # Which branch: a reposition draws rand(0x8000), a break-off pushes
        # VTOL_Evade, a pursuit installs the controller with neither.
        kind_of = 'formation' if formations else 'step-out' if steps else 'evade' if evades else \
            'reposition' if moving and 0x8000 in bounds else \
            'pursuit' if moving else 'off-map' if points else 'other'
        stats[kind_of] = stats.get(kind_of, 0) + 1
    compare('41ca50', fixtures, expected, run('--air-to-air', fixtures), left, (13, 15))
    print('41ca50: %d handler calls match %s; group check %s; %d lost-target VTOL_SEEKATTACK pushes '
          '(check_air_seek_attack.py)' % (len(fixtures), stats, group_stats, seeks))
    group_stats.clear()

    # --- VTOL_Evade 41e3d0 -----------------------------------------------
    fixtures, expected, left = [], [], []
    stats = {}
    put(target_mover + 0x36, 'H', 0)
    for i in range(12000):
        stage = rng.choice((0, 0, 0, 1, 1, 1, 1, 1, 1, 2))
        events = rng.choice((0, 0, 0, 1, 1, 0x100, 0x8, 0x20, 0x40, 0x41, 0x700))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x3700, rng.randrange(2**32)))
        flags = rng.choice((0, 0x1000200, 0x8000000))
        maneuver = rng.choice((0, 0, 1, 2, 3, 4, 4, 5, 6, 7, -1))
        budget = rng.choice((0, -3, 1, 4, 5, 9, 10, 19, 20, 33, 34, 50, 88, rng.randrange(-20, 120)))
        width = rng.choice((4096 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, rng.randrange(256, 12000)))
        x = rng.randrange(0, width) * 65536 + rng.randrange(65536)
        z = rng.randrange(0, height) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536
        weapon_reach = rng.choice((0, 0, 30, rng.randrange(65536)))
        unit_reach = rng.choice((100, 212, 0, 2, rng.randrange(1000)))
        reach = weapon_reach or unit_reach
        spread = rng.choice((0, 1, 2, 30, reach + 1, 3 * reach + 1, 2000))
        tx = x + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        tz = z + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        heading = rng.randrange(65536)
        max_speed = rng.choice((rng.randrange(65536, 6 * 65536), 229376, 252314, 7 * 65536))
        type_speed = rng.choice((rng.randrange(1, 8 * 65536), 3 * 65536, 229376))
        target_type_speed = rng.choice((rng.randrange(1, 8 * 65536), type_speed * 4 // 5, type_speed // 2,
                                        type_speed // 2 + 1, 255590))
        turn_rate = rng.choice((115, 200, 500, 700, rng.randrange(65536)))
        target_turn_rate = rng.choice((200, 500, turn_rate * 4 // 5, rng.randrange(65536)))
        flies = 0 if i % 43 == 0 else 1
        on_map = rng.random() < 0.95
        seed = rng.randrange(2**32)
        answers.update(out=rng.random() < 0.2, inserted=rng.choice((0, 1, 1, 1)))
        nxt = rng.choice((None, 0x1000200, 0x1000200, 0x3000400, 0x5012000, 0))

        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(unit + 0x130, 'I', 0x9000000)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0x7e, 'H', heading)
        put(unit + 0xa4, 'I', sector if on_map else offmap)
        p.uc.mem_write(unit + 0x12b, struct.pack('<i', max_speed))
        put(kind + 0x260, 'I', 0x800 if flies else 0)
        put(kind + 0x162, 'i', type_speed)
        put(kind + 0x18e, 'H', turn_rate)
        put(kind + 0x25a, 'H', unit_reach)
        put(weapon + 0xc6, 'H', weapon_reach)
        put(target + 0x130, 'I', 0x1000002)
        put(target + 0x68, 'iii', s32(tx), 0, s32(tz))
        put(target_kind + 0x162, 'i', target_type_speed)
        put(target_kind + 0x18e, 'H', target_turn_rate)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x16, 'I', target)
        put(mission + 0x4e, 'i', maneuver)
        put(mission + 0x52, 'i', budget)
        put(mission + 0x56, 'I', 0)
        put(mission + 0x5a, 'I', flags)
        group_case(nxt)
        put(mission + 0x6a, 'I', pending)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        bounds.clear()
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = p.call(0x41e3d0, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        left.append(watch.left())
        installs = [c for c in log.get('install', []) if c]
        points, moving = log.get('point', []), log.get('moving', [])
        formations = log.get('descriptor', []).count('VTOL_Move_Formation')
        if len(installs) != len(points) + len(moving) or len(log.get('descriptor', [])) != formations:
            raise AssertionError((i, 'controllers', log))
        group_outcome(s32(result), formations)
        goal = points[-1] if points else (0, 0, 0)
        goal_radius = log['radius'][-1] if points else 0
        if moving:
            (mx, my, mz), velocity = moving[-1]
            if velocity != (0, 0, 0) or not get(moving_goal + 8, 'H') & 1:
                raise AssertionError((i, 'evade turn controller', moving))
            mh = get(moving_goal + 0x24, 'H')
        else:
            mx = my = mz = mh = 0
        expected.append((s32(result), get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa),
                         get(mission + 0x6a), get(mission + 0x5a), get(mission + 0x4e, 'i'),
                         get(mission + 0x52, 'i'), len(points), len(moving), *goal, goal_radius, mx, my, mz, mh,
                         get(RNG), draws[0], len(log.get('group', [])), len(log.get('leave', [])), formations))
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, maneuver, budget, s32(x),
                                           s32(y), s32(z), heading, s32(tx), 0, s32(tz), max_speed, type_speed,
                                           target_type_speed, turn_rate, target_turn_rate, reach, flies,
                                           int(on_map), width, height, seed, int(answers['out']), nxt or 0,
                                           answers['inserted']))))
        key = ('stage%d' % stage, 'formation' if formations else
               'maneuver%d' % get(mission + 0x4e, 'i') if stage == 0 and result == 1
               else 'turn' if moving else 'line' if points else 'r%d' % s32(result))
        stats[key] = stats.get(key, 0) + 1
    compare('41e3d0', fixtures, expected, run('--air-evade', fixtures), left)
    print('41e3d0: %d handler calls match; group check %s' % (len(fixtures), group_stats))
    for key in sorted(stats):
        print('   %s: %d' % (' '.join(key), stats[key]))


def dropped_gate(binary):
    """52ae90 runs aim start (52fe30) and the readiness gate (52fff0) for every
    weapon class. Through DroppedBallisticWeapon's own vtable (5f3ac4), with
    the real geometry 52fd80: its angle virtual (+0x28, 4d4450) is a bare
    ret, so the record keeps its angles, AimWeapon starts with them, the
    gate's angle test passes, and the bomb waits for SET 22 and, on a hover
    attacker, for its facing request. Hooked: the muzzle query 4dd420, the
    target lookup 51a9a0, the led SweetSpot 51aa50, the range predicates and
    the script callback 56c640; 4dc0b0, 51b340 and 5193c0 run natively."""
    p = Icd()
    unit, kind, mover, navigator, nav_vtable, weapon, handler, target = [HEAP + i * 0x1000 for i in range(8)]
    notify_stub = HEAP + 0x9000
    record = unit + 12
    started = []

    def put(address, fmt, *values):
        p.uc.mem_write(address, struct.pack('<' + fmt, *values))

    def get(address, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    led = [(0, 0, 0)]

    def muzzle(uc, args):
        put(get(args + 4), '3i', 0, 0, 0)
        return 4, 0

    def lead(uc, args):
        put(get(args + 4), '3i', *led[0])
        return 3, 1

    def callback(uc, args):
        values = struct.unpack('<8I', uc.mem_read(args, 32))
        name = bytes(uc.mem_read(values[0], 32)).split(b'\0')[0].decode()
        started.append((name, values[4], values[5]))
        return 8, 0

    p.hooks[0x4dd420] = muzzle
    p.hooks[0x51aa50] = lead
    p.hooks[0x51a9a0] = lambda uc, a: (2, target)
    p.hooks[0x5306a0] = lambda uc, a: (3, 0)
    p.hooks[0x532290] = lambda uc, a: (3, 1)
    p.hooks[0x56c640] = callback
    p.hooks[0x4ea4f0] = lambda uc, a: (4, 0)
    p.hooks[notify_stub] = lambda uc, a: (0, 0)
    p.freeze_hooks()
    put(unit + 8, 'I', mover)
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xd8, 'f', 10.0)
    put(unit + 0xf4, 'f', 1.0)
    put(record, 'I', weapon)
    put(weapon + 0x40, 'I', handler)
    put(weapon + 0xd4, 'f', 1.0)
    put(handler, 'I', 0x5f3ac4)
    put(mover, 'I', navigator)
    put(navigator, 'I', nav_vtable)
    put(nav_vtable + 0x38, 'I', notify_stub)
    rng = random.Random(0x5f3ac4)
    counts = {'started': 0, 'released': 0, 'held by SET 22': 0, 'held by facing': 0}
    for i in range(6000):
        hover = rng.randrange(2)
        put(kind + 0x260, 'I', 0x800 | (0x8000000 if hover else 0))
        put(kind + 0x190, 'H', 0)
        x, z = rng.randrange(2**28), rng.randrange(2**28)
        put(unit + 0x68, '3i', x, 0, z)
        led[0] = (x + rng.randrange(-400, 401) * 65536, 0, z + rng.randrange(-400, 401) * 65536)
        old = (rng.randrange(65536), rng.randrange(65536))
        flags = rng.randrange(65536)
        tolerance = rng.choice((0, 300, 512, 1024, rng.randrange(65536)))
        put(weapon + 0xc0, 'H', tolerance)
        put(record + 0x14, 'H', 0)
        put(record + 0x16, '3H', *old, flags)
        put(mover + 0x34, 'H', rng.randrange(65536))
        put(mover + 0x36, 'H', rng.randrange(65536))
        started.clear()
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = p.call(0x52fe30, (unit, record), ecx=handler)
        if error or result != 1:
            raise AssertionError((i, error, result))
        start = not (flags & 0xe0)
        if start:
            counts['started'] += 1
            if started != [('AimWeapon', old[0], old[1])]:
                raise AssertionError((i, 'AimWeapon', started, old))
        elif started:
            raise AssertionError((i, 'restarted', started))
        if struct.unpack('<2H', p.uc.mem_read(record + 0x16, 4)) != old:
            raise AssertionError((i, 'aim start moved the angles'))
        request = get(mover + 0x34, 'H')
        # The script acknowledges (SET 22) or not; the body faces anywhere.
        ack = rng.randrange(2)
        flags = get(record + 0x1a, 'H')
        put(record + 0x1a, 'H', (flags | 8) if ack else (flags & ~8))
        heading = (request + rng.choice((0, 511, 512, 513, -512, -513, rng.randrange(65536)))) & 0xffff
        put(unit + 0x7e, 'H', heading)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        ready, error = p.call(0x52fff0, (unit, record), ecx=handler)
        if error:
            raise AssertionError((i, error))
        off = abs(((heading - request + 32768) & 65535) - 32768)
        aligned = not hover or off <= max(tolerance, 512)
        if ready != int(ack and aligned):
            raise AssertionError((i, 'gate', ready, ack, aligned, hover, off, tolerance))
        if struct.unpack('<2H', p.uc.mem_read(record + 0x16, 4)) != old:
            raise AssertionError((i, 'the gate moved the angles'))
        counts['released' if ready else 'held by SET 22' if not ack else 'held by facing'] += 1
    print('DroppedBallisticWeapon 52fe30 + 52fff0: %d aim starts and gates as World runs them %s' % (6000, counts))


if __name__ == '__main__':
    main()
    dropped_gate(sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_motion_test')
