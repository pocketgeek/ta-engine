#!/usr/bin/env python3
"""Compare retail's air traffic with src/sim/retailairtraffic.h,
retailAirToGroundHover and retailAirStrike (src/sim/retailairattack.h) and
retailVtolSeekGuard (src/sim/retailairseek.h).

1. The counter step (51dc4b..51dce4), run natively over the loop body for one
   live, not dying flyer, against retailAirTrafficStep for every counter and
   overlap count (65,536 pairs); a dead, a dying and a grounded unit's counter
   is left alone (the World caller's gates).
2. VTOL_StepOut (4214b0), the handler of all three step-outs, on random states
   of both stages and the head: the octant scores (eight draws) against up to
   seven overlapping flyers (41d780, live or dead, in an entity table of
   stride 0x138), the distance draws, the angle, the point controller and its
   radius, the off-map return, the mover and canfly exits. Controller
   allocation, construction, the radius setter and installation are stubs
   that record their arguments; installation clears the pending controller
   events as 4d4d40 does.
3. AirToGroundHover (41d820) on random states of every stage and the head
   past its target checks: the lost-target exit (events 0x28), the off-map
   return, the restart (0x40), the group check (51d1e0, answered) with the
   leave (50c480) and the VTOL_Move_Formation push, the flag's rand(25);
   stage 0's take-off, stage 1's approach controller (D + 144) or advance,
   stage 2's slot calls and led SweetSpot, stage 3's ring point, the target
   controller, its radius and the 15..29-tick sleep, stage 4's step-out (the
   count, the auto retarget, D, the give-up) and wasted-shot reposition, and
   stage 5's random ring point. The slot calls, the scan (4d8370), the attack
   (4d7de0), the mission lookups, constructors and pushes (4d4bf0, 4d6c40,
   4d7750, 4d77b0) are stubs that answer as the fixture says and record their
   arguments; the sleeps (4d6a10), _ftol and every draw (535cc0, state at
   0x64186c) are native.
4. retailAirTrafficCheck against the check in each of its ten missions' own
   handlers (VTOL_Standby, VTOL_LandIfCan, VTOL_Move_Formation, VTOL_Move,
   VTOL_Follow, VTOL_SeekGuard, VTOL_Patrol, VTOL_MobileBuild, VTOL_HelpBuild,
   VTOL_RepairUnit), each run from its entry past its head's exits and
   stopped where it goes on without a step-out (check_mission_checks).
5. VTOL_SeekGuard (4197d0) whole on random states (check_seek_guard).
6. AirStrike (41b5e0) whole on random states, the release ring's x87
   sequence native, the group check (51d1e0) answered in or out of the slot
   with the next mission (+0x66) a group mover or not, the leave (50c480)
   recorded (check_air_strike).

    python3 tools/re/check_air_traffic.py [build-dbg/retail_motion_test]
"""
import random
import struct
import subprocess
import sys

import emurange
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_EDI, \
    UC_X86_REG_EDX, UC_X86_REG_ECX

RNG = 0x64186c


def s32(v):
    return (v + 2**31) % 2**32 - 2**31


def run(binary, mode, lines):
    output = subprocess.run([binary, mode], input='\n'.join(lines) + '\n', text=True, capture_output=True, check=True)
    return [tuple(map(int, line.split())) for line in output.stdout.splitlines()]


def compare(name, fixtures, expected, actual, left=None, point=()):
    """Every row equal. `left`, per fixture: where retail left the 16.16 range in that call (emurange.Watch); the
    port stops there instead (ours), so such a row may differ in the x and z of the point it builds (`point`,
    the last thing these missions build), and in nothing else."""
    if len(actual) != len(expected):
        raise AssertionError('%s: row count %d, expected %d' % (name, len(actual), len(expected)))
    past = 0
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            if left and left[i] and emurange.stopped(got, want, point):
                past += 1
                continue
            raise AssertionError('%s fixture %d: %s\n  port   %s\n  native %s' % (name, i, fixtures[i], got, want))
    if left:
        print('%s: retail leaves the range in %d fixtures; the port differs in %d of them, in the built point '
              'alone (ours)' % (name, sum(map(bool, left)), past))


def check_counter(binary):
    p = Icd()
    unit, kind, game = HEAP, HEAP + 0x1000, HEAP + 0x20000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(unit + 0xb4, 'I', kind)

    def native(counter, overlaps, flags=0x1000000, canfly=True):
        put(unit + 0x130, 'I', flags)
        put(kind + 0x260, 'I', 0x800 if canfly else 0)
        put(unit + 0x12a, 'b', counter)
        put(unit + 0x117, 'b', overlaps)
        # One pass of the per-player loop body: the unit is the range's last.
        p.uc.reg_write(UC_X86_REG_EAX, unit)
        p.uc.reg_write(UC_X86_REG_EDI, unit)
        p.uc.reg_write(UC_X86_REG_EBX, 0x800)
        p.uc.reg_write(UC_X86_REG_EDX, game)
        p.uc.emu_start(0x51dc4b, 0x51dcf7, count=200)
        return get(unit + 0x12a, 'b')

    pairs = [(c, o) for c in range(-128, 128) for o in range(-128, 128)]
    expected = [native(c, o) for c, o in pairs]
    fixtures = ['%d %d' % pair for pair in pairs]
    actual = [row[0] for row in run(binary, '--air-traffic', fixtures)]
    for i, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError('counter %s: port %d, native %d' % (fixtures[i], got, want))
    # The gates are the caller's: a dead (no 0x1000000), dying (0x1000) or
    # grounded type's counter is not touched.
    for flags, canfly in ((0, True), (0x1001000, True), (0x1000000, False)):
        if native(7, 3, flags, canfly) != 7:
            raise AssertionError('counter gate %#x canfly=%d' % (flags, canfly))
    print('PASS: air-traffic counter, %d counter/overlap pairs and the three gates' % len(pairs))


def check_step_out(binary):
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game = HEAP + 0x20000
    entities = HEAP + 0x40000          # stride 0x138
    mission, kind, offmap, controller = HEAP, HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(game + 0x19f30, 'I', offmap)
    put(game + 0x14e84, 'I', entities)
    put(game + 0x14e88, 'I', entities + 0x138 * 40)
    log = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        if get(args) != 0x36:
            raise AssertionError('unexpected allocation of %#x bytes' % get(args))
        return 0, controller

    def construct(uc, args):   # 4e40e0(mission, point), thiscall
        if get(args) != mission:
            raise AssertionError('controller for another mission')
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 4), 12)))
        put(controller + 0xa, 'h', 0)
        put(controller + 8, 'H', 0x20)
        record('construct')
        return 2, controller

    def radius(uc, args):      # 4e4540(radius), thiscall
        put(controller + 0xa, 'H', get(args) & 0xffff)
        put(controller + 8, 'H', get(controller + 8, 'H') | 0x10)
        return 1, 0

    def install(uc, args):     # 4d4d40(controller), thiscall on the mission
        record('install', get(args))
        put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = construct
    p.hooks[0x4e4540] = radius
    p.hooks[0x4d4d40] = install
    p.freeze_hooks()
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1), begin=0x535cc0, end=0x535cc0)

    def entity(i):
        return entities + 0x138 * i

    rng = random.Random(0x4214b0)
    fixtures, expected = [], []
    stages = {}
    for i in range(3000):
        stage = rng.choice((0, 0, 0, 0, 0, 1, 1, 2))
        events = rng.choice((0, 0x100, 0x200, 0x400, 0x500, 1))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, rng.randrange(2**32)))
        flags = rng.choice((0x1000000, 0x5000000, 0))
        width = rng.choice((4096 - 32, 8192 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, 8192 - 128, rng.randrange(256, 12000)))
        on_map = rng.random() < 0.85
        flies = rng.random() < 0.95
        x = rng.randrange(-300, width + 300) * 65536 + rng.randrange(65536)
        z = rng.randrange(-300, height + 300) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536 + rng.randrange(65536)
        heading = rng.randrange(65536)
        foot = rng.choice((1, 2, 3, 4, 6))
        distance = rng.choice(((rng.randrange(15) + foot) * 16,
                               (rng.randrange(10) + rng.randrange(10) + rng.randrange(10) + foot) * 16,
                               rng.randrange(0, 40)))
        point = (s32(rng.randrange(2**32)), s32(rng.randrange(2**32)), s32(rng.randrange(2**32)))
        seed = rng.randrange(2**32)
        n = rng.choice((0, 1, 1, 2, 3, 5, 7, -1))
        # The unit is entity 1; its listed overlaps are entities 2.. (some dead).
        me = entity(1)
        put(me + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(me + 0x7e, 'H', heading)
        put(me + 0xb4, 'I', kind)
        put(me + 8, 'I', 1 if flies or rng.random() < 0.5 else 0)
        mover = get(me + 8)
        put(kind + 0x260, 'I', 0x800 if (flies or not mover) else 0)
        flies_port = bool(mover) and bool(get(kind + 0x260) & 0x800)
        put(me + 0xa4, 'I', offmap + 4 if on_map else offmap)
        put(me + 0x117, 'b', n)
        live = []
        for k in range(max(n, 0)):
            e = entity(2 + k)
            ox = x + rng.randrange(-48, 49) * 65536 + rng.randrange(65536)
            oz = z + rng.randrange(-48, 49) * 65536 + rng.randrange(65536)
            oy = rng.randrange(0, 512) * 65536
            alive = rng.random() < 0.85
            put(e + 0x68, 'iii', s32(ox), s32(oy), s32(oz))
            put(e + 0x130, 'I', 0x1000000 | (0x1000 if rng.random() < 0.1 else 0) if alive else 0)
            put(me + 0x118 + 2 * k, 'H', 2 + k)
            if alive:
                live.append((s32(ox), s32(oy), s32(oz)))
        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x22, 'iii', *point)
        put(mission + 0x4e, 'i', distance)
        put(mission + 0x5a, 'I', flags)
        put(mission + 0x6a, 'I', pending)
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        put(controller + 8, 'H', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        result, error = p.call(0x4214b0, (me, mission, events))
        if error:
            raise AssertionError((i, error))
        installs = len(log.get('install', []))
        goal = struct.unpack('<3i', p.uc.mem_read(controller + 0x26, 12)) if installs else (0, 0, 0)
        expected.append((result, get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa), get(mission + 0x6a),
                         get(mission + 0x5a), *struct.unpack('<3i', p.uc.mem_read(mission + 0x22, 12)), installs,
                         *goal, get(controller + 0xa, 'h') if installs else 0,
                         get(controller + 8, 'H') if installs else 0x20, get(RNG), draws[0]))
        fields = [stage, events, tick, deadline, pending, flags, *point, s32(x), s32(y), s32(z), heading, distance,
                  int(flies_port), int(on_map), width, height, seed, len(live)]
        for o in live:
            fields += list(o)
        fixtures.append(' '.join(map(str, fields)))
        key = 'off' if not on_map else stage
        stages[key] = stages.get(key, 0) + 1
    compare('step-out', fixtures, expected, run(binary, '--step-out', fixtures))
    print('PASS: VTOL_StepOut, %d calls (%s), every state, point, controller, radius and draw'
          % (len(fixtures), ' '.join('%s:%d' % kv for kv in sorted(stages.items(), key=str))))


def check_hover(binary):
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mission, kind, weapon, target, offmap, controller, block, found, lead_at = \
        [HEAP + i * 0x1000 for i in range(10)]
    game = HEAP + 0x20000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(game + 0x19f30, 'I', offmap)
    log = {}
    answer = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        size = get(args)
        if size == 0x36:
            return 0, controller
        if size == 0x72:
            return 0, block
        raise AssertionError('unexpected allocation of %#x bytes' % size)

    def point_controller(uc, args):    # 4e40e0(mission, point)
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 4), 12)))
        put(controller + 0xa, 'h', 0)
        record('approach')
        return 2, controller

    def target_controller(uc, args):   # 4e4140(mission, target, point)
        if get(args + 4) != target:
            raise AssertionError('target controller on another unit')
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 8), 12)))
        put(controller + 0xa, 'h', 0)
        record('ring')
        return 3, controller

    def radius(uc, args):
        put(controller + 0xa, 'H', get(args) & 0xffff)
        return 1, 0

    def install(uc, args):
        record('install', get(args))
        put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    def lookup(uc, args):              # 4d4bf0(name): the row slot is ecx
        record('lookup', bytes(uc.mem_read(get(args), 32)).split(b'\0')[0].decode())
        return 1, uc.reg_read(UC_X86_REG_ECX)

    def construct(uc, args):           # 4d6c40(row, target, point, +0x4e, +0x52, +0x56, vec, vec)
        record('mission', (log['lookup'][-1], get(args + 4), get(args + 12, 'i'), get(args + 16, 'i'),
                           get(args + 20, 'i')))
        put(block + 4, 'B', 1 if answer['built'] else 0)
        return 12, block

    def lead(uc, args):                # 51aa50(unit, point, slot)
        p.uc.mem_write(get(args + 4), bytes(p.uc.mem_read(lead_at, 12)))
        record('lead')
        return 3, 0

    def scan(uc, args):                # 4d8370(unit)
        record('scan')
        return 1, found if answer['scan'] else 0

    def attack(uc, args):              # 4d7de0(unit, found, 0, 0, 1, 0)
        if (get(args + 4), get(args + 8), get(args + 12), get(args + 16), get(args + 20)) != (found, 0, 0, 1, 0):
            raise AssertionError('unexpected 4d7de0 arguments')
        record('attack')
        return 6, 1 if answer['attack'] else 0

    def group(uc, args):               # 51d1e0(unit, 1, 5)
        if (get(args + 4), get(args + 8)) != (1, 5):
            raise AssertionError('unexpected 51d1e0 arguments')
        record('group')
        return 3, 1 if answer['group'] else 0

    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = point_controller
    p.hooks[0x4e4140] = target_controller
    p.hooks[0x4e4540] = radius
    p.hooks[0x4e44c0] = lambda uc, a: (record('altitude', get(a)), (1, 0))[1]
    p.hooks[0x4d4d40] = install
    p.hooks[0x4d4bf0] = lookup
    p.hooks[0x4d6c40] = construct
    p.hooks[0x4d7750] = lambda uc, a: (record('push'), (2, 0))[1]
    p.hooks[0x4d77b0] = lambda uc, a: (record('insert'), (2, 0))[1]
    p.hooks[0x4d6da0] = lambda uc, a: (record('destroy'), (0, 0))[1]
    p.hooks[0x4eba00] = lambda uc, a: (record('free'), (0, 0))[1]
    p.hooks[0x50c480] = lambda uc, a: (record('leave'), (1, 0))[1]
    p.hooks[0x51d1e0] = group
    p.hooks[0x416c50] = lambda uc, a: (record('begin'), (4, 0))[1]
    p.hooks[0x519ac0] = lambda uc, a: (0, 0)
    p.hooks[0x519b10] = lambda uc, a: (record('auto', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x519b70] = lambda uc, a: (record('manual', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x51a700] = lambda uc, a: (record('aim', (get(a + 4), get(a + 8) & 0xff)), (3, 0))[1]
    p.hooks[0x51aa50] = lead
    p.hooks[0x4d8370] = scan
    p.hooks[0x4d7de0] = attack
    for address, name in [(0x51a770, 'ground aim'), (0x4e4520, 'controller heading')]:
        p.hooks[address] = (lambda n: lambda uc, a: (_ for _ in ()).throw(AssertionError('unexpected ' + n)))(name)
    p.freeze_hooks()
    # Sees retail leave the 16.16 range in a call (emurange.py): a sum of positions wraps, or the distance to the
    # point (41dd5f) is 32768 px or more and reads negative.
    watch = emurange.Watch(p, ((0x41d820, 0x41e3cf),) + emurange.HELPERS, words=(0x41dd5f,))
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1), begin=0x535cc0, end=0x535cc0)
    lost = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: lost.__setitem__(0, lost[0] + 1), begin=0x41e28f, end=0x41e28f)

    # A hover attacker with a weapon (+0x130 0x8000000), a canfly hoverattack
    # type, slot 0 not `dropped`, D and the altitude on the type; a live
    # target on the map, not carried.
    put(unit + 0x130, 'I', 0x9000000 | 0x80000)   # Fire At Will: a lost target may push the seek
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xc, 'I', weapon)
    put(kind + 0x1aa, 'I', weapon)
    put(weapon + 0xc8, 'I', 0)
    put(weapon + 0xc2, 'H', 0)
    put(weapon + 0xc4, 'h', 0)
    put(target + 0x114, 'B', 0)

    rng = random.Random(0x41d820)
    fixtures, expected, left = [], [], []
    stages = {}
    for i in range(8000):
        stage = rng.choice((0, 1, 1, 2, 3, 3, 4, 4, 4, 4, 5, 5, 6))
        events = rng.choice((1, 1, 1, 0x100, 0x400, 0x500, 0x200000, 0x400000, 0x800000, 0x10000, 0x40, 0x8, 0x20, 0x28,
                             0x200001, 0x700))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, rng.randrange(2**32)))
        flags = rng.choice((0x1000200, 0x1000200, 0x9000200, 0x8000000))
        count = rng.choice((0, 0, 1, 2, 3, 5, 9, 12))
        width = rng.choice((4096 - 32, 8192 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, 8192 - 128, rng.randrange(256, 12000)))
        on_map = rng.random() < 0.85
        flies = rng.random() < 0.95
        x = rng.randrange(-300, width + 300) * 65536 + rng.randrange(65536)
        z = rng.randrange(-300, height + 300) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536 + rng.randrange(65536)
        spread = rng.choice((0, 16, 150, 400, 900))
        tx = x + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        tz = z + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        ty = rng.randrange(0, 256) * 65536
        lx = tx + rng.randrange(-40, 41) * 65536
        lz = tz + rng.randrange(-40, 41) * 65536
        ly = ty + rng.randrange(0, 60) * 65536
        point = (s32(rng.randrange(2**32)), s32(rng.randrange(2**32)), s32(rng.randrange(2**32)))
        if i % 5 == 0:
            point = (s32(lx), s32(ly), s32(lz))
        distance = rng.choice((0, 50, 100, 150, 200, 280, rng.randrange(0, 600)))
        altitude = rng.choice((0, 50, 100, 150))
        still = rng.random() < 0.6
        automatic = rng.random() < 0.5
        traffic = rng.choice((0, 2, 4, 5, 5, 7, 12, 17))
        foot = rng.choice((1, 2, 3, 4, 6))
        seed = rng.randrange(2**32)
        answer.update(group=rng.random() < 0.15, built=rng.random() < 0.95, scan=rng.random() < 0.5,
                      attack=rng.random() < 0.5)
        mover = 1 if flies or rng.random() < 0.5 else 0
        put(unit + 8, 'I', mover)
        put(kind + 0x260, 'I', 0x8000000 | (0x800 if (flies or not mover) else 0))
        flies_port = bool(mover) and bool(get(kind + 0x260) & 0x800)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0x78, 'h', foot)
        put(unit + 0xa4, 'I', offmap + 4 if on_map else offmap)
        put(unit + 0x12a, 'b', traffic)
        put(kind + 0x256, 'H', distance)
        put(kind + 0x258, 'h', altitude)
        put(target + 0x68, 'iii', s32(tx), s32(ty), s32(tz))
        put(target + 8, 'I', 0 if still and rng.random() < 0.5 else 1)
        put(target + 0x130, 'I', 0x1000001 | (0 if still or not get(target + 8) else 4))
        if not still and not get(target + 8):
            put(target + 8, 'I', 1)
            put(target + 0x130, 'I', 0x1000005)
        put(lead_at, 'iii', s32(lx), s32(ly), s32(lz))
        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x16, 'I', target)
        put(mission + 0x22, 'iii', *point)
        put(mission + 0x4e, 'i', count)
        put(mission + 0x52, 'I', 1 if automatic else 0)
        put(mission + 0x56, 'I', 0)
        put(mission + 0x5a, 'I', flags)
        put(mission + 0x66, 'I', 0)
        put(mission + 0x6a, 'I', pending)
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = lost[0] = 0
        result, error = p.call(0x41d820, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        left.append(watch.left())
        if log.get('auto') and any(a != 3 for a in log['auto']):
            raise AssertionError((i, 'auto slots', log['auto']))
        if log.get('manual') and any(a != 0 for a in log['manual']):
            raise AssertionError((i, 'manual slot', log['manual']))
        if log.get('aim') and any(a != (target, 0) for a in log['aim']):
            raise AssertionError((i, 'aim', log['aim']))
        missions = log.get('mission', [])
        steps = [m for m in missions if m[0] == 'VTOL_StepOut']
        formations = [m for m in missions if m[0] == 'VTOL_Move_Formation']
        for m in formations:
            # target 0, +0x4e 0, +0x52 0 (the hover's +0x52 is below 100), +0x56 -4
            if m[1:] != (0, 0, 0, -4):
                raise AssertionError((i, 'formation arguments', m))
        controllers = len(log.get('approach', [])) + len(log.get('ring', []))
        goal = struct.unpack('<3i', p.uc.mem_read(controller + 0x26, 12)) if controllers else (0, 0, 0)
        built = answer['built']
        expected.append((result, get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa), get(mission + 0x6a),
                         get(mission + 0x5a), get(mission + 0x4e, 'i'),
                         *struct.unpack('<3i', p.uc.mem_read(mission + 0x22, 12)),
                         len(log.get('approach', [])), len(log.get('ring', [])), *goal,
                         get(controller + 0xa, 'h') if controllers else 0,
                         lost[0], len(log.get('group', [])), len(log.get('leave', [])), len(formations),
                         len(log.get('begin', [])), len(log.get('auto', [])), len(log.get('manual', [])),
                         len(log.get('lead', [])), len(log.get('scan', [])), len(steps),
                         steps[-1][2] if steps else 0, get(RNG), draws[0]))
        # The port's retarget() is the scan and the attack together: it answers
        # yes only when both do.
        retargets = int(answer['scan'] and answer['attack'])
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, count, s32(x), s32(y), s32(z),
                                           s32(tx), s32(ty), s32(tz), s32(lx), s32(ly), s32(lz), *point, distance,
                                           int(flies_port), 1, int(still), int(automatic), traffic, foot,
                                           int(on_map), width, height, seed, int(answer['group']), int(built),
                                           retargets, int(built), 5))))
        key = 'off' if not on_map else stage
        stages[key] = stages.get(key, 0) + 1
    rows = run(binary, '--air-hover', fixtures)
    # The port's row: result stage waitMask deadline pending flags count point(3) approaches rings goal(3) radius
    # flags heading lost out leaves formations begin auto aims targetPoints leads retargets stepOuts stepOutD seed calls.
    actual = []
    for r in rows:
        (result, stage, wait, deadline, pending, flags, count, px, py, pz, approaches, rings, gx, gy, gz, radius,
         gflags, gheading, lostc, outc, leaves, formations, begin, autos, aims, targets, leads, retargets, steps,
         stepd, seed, calls) = r
        actual.append((result, stage, wait, deadline, pending, flags, count, px, py, pz, approaches, rings, gx, gy, gz,
                       radius, lostc, outc, leaves, formations, begin, autos, aims, leads, retargets, steps, stepd,
                       seed, calls))
    compare('hover', fixtures, expected, actual, left, (12, 14))
    def tally(column):
        return sum(1 for row in expected if row[column])
    print('PASS: AirToGroundHover, %d calls (%s), every state, point, controller, slot call, push and draw '
          '(%d approaches, %d ring points, %d lost, %d leaves, %d formations, %d rescans, %d step-outs, '
          '%d ended (8))'
          % (len(fixtures), ' '.join('%s:%d' % kv for kv in sorted(stages.items(), key=str)), tally(10), tally(11),
             tally(16), tally(18), tally(19), tally(24), tally(25), sum(1 for row in expected if row[0] == 8)))


# The missions retailAirTrafficCheck covers, in RetailTrafficMission order:
# (name, handler, the address the handler goes on at when it pushes no
# step-out). Each handler is called with its head's own exits passed, so it
# either reaches that address (no step-out: the emulation stops there) or
# returns from the check.
TRAFFIC_MISSIONS = [
    ('VTOL_Standby', 0x417350, 0x417527),
    ('VTOL_LandIfCan', 0x416cd0, 0x416eda),
    ('VTOL_Move_Formation', 0x417750, 0x41794c),
    ('VTOL_Move', 0x417c20, 0x417e02),
    ('VTOL_Follow', 0x418860, 0x418a7b),
    ('VTOL_SeekGuard', 0x4197d0, 0x4199b2),
    ('VTOL_Patrol', 0x419db0, 0x419f9b),
    ('VTOL_MobileBuild', 0x41ef00, 0x41f169),
    ('VTOL_HelpBuild', 0x41f7f0, 0x41fa44),
    ('VTOL_RepairUnit', 0x420e60, 0x421129),
]
STEP_OUT_KINDS = {'VTOL_StepOut': 1, 'VTOL_StepOutStandby': 2, 'VTOL_StepOutNoFormation': 3}


def check_mission_checks(binary):
    """retailAirTrafficCheck against every mission's own handler: the counter
    at -4..13 (and the byte's extremes), every stage, the push built or not,
    the formation's 0x4000000; the step-out's name, its D (the constructor's
    +0x4e argument), the push, the sleep, the stage, the result, a flying
    builder's 51e4d0(8, 0), the draw and the generator state."""
    p = Icd()
    p.allow_early_stop = True
    unit, mission, kind, game, block, ward, offmap = [HEAP + i * 0x1000 for i in range(7)]
    game = HEAP + 0x20000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(game + 0x19f30, 'I', offmap)
    log = {}
    answer = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        if get(args) != 0x72:
            raise AssertionError('unexpected allocation of %#x bytes' % get(args))
        return 0, block

    def lookup(uc, args):              # 4d4bf0(name): the row slot is ecx
        record('lookup', bytes(uc.mem_read(get(args), 40)).split(b'\0')[0].decode())
        slot = uc.reg_read(UC_X86_REG_ECX)
        uc.mem_write(slot, b'\x2a')
        return 1, slot

    def construct(uc, args):           # 4d6c40(row, target, point, +0x4e, +0x52, +0x56, vec, vec)
        record('mission', (log['lookup'][-1], get(args + 12, 'i')))
        put(block + 4, 'B', 1 if answer['built'] else 0)
        return 12, block

    def flag(uc, args):                # 51e4d0(flag, on), thiscall on the unit
        record('flag', (get(args), get(args + 4)))
        return 2, 0

    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4d4bf0] = lookup
    p.hooks[0x4d6c40] = construct
    p.hooks[0x4d7750] = lambda uc, a: (record('push'), (2, 0))[1]
    p.hooks[0x4d6da0] = lambda uc, a: (record('destroy'), (0, 0))[1]
    p.hooks[0x4eba00] = lambda uc, a: (record('free'), (0, 0))[1]
    p.hooks[0x51e4d0] = flag
    p.freeze_hooks()
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1), begin=0x535cc0, end=0x535cc0)
    went_on = [False]

    def stop(uc, address, size, data):
        went_on[0] = True
        uc.emu_stop()
    for _, _, onward in TRAFFIC_MISSIONS:
        p.uc.hook_add(UC_HOOK_CODE, stop, begin=onward, end=onward)

    # A flyer on the map, alive; its guarded unit, site or repair target alive
    # and not dying, a repair target the right kind (+0x130 & 3 == 1).
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xa4, 'I', offmap + 4)
    put(unit + 8, 'I', 1)
    put(unit + 0x130, 'I', 0x1000000)
    put(kind + 0x260, 'I', 0x800)
    put(ward + 0xb4, 'I', kind)
    put(ward + 0x130, 'I', 0x1000001)
    rng = random.Random(0x12a)
    fixtures, expected = [], []
    tally = {}
    traffics = list(range(-4, 14)) + [127, -128]
    for index, (name, handler, onward) in enumerate(TRAFFIC_MISSIONS):
        for i in range(1600):
            stage = i % 8 if i < 320 else rng.randrange(8)
            traffic = traffics[(i // 8) % len(traffics)] if i < 320 else rng.choice(traffics)
            foot = rng.choice((1, 2, 3, 4, 6, 8))
            tick = rng.randrange(1000000)
            deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
            pending = rng.choice((0, 0x100, 0x500, rng.randrange(2**32)))
            flags = rng.choice((0x1000000, 0x5000000, 0x1000400, rng.randrange(2**32)))
            seed = rng.randrange(2**32)
            answer['built'] = rng.random() < 0.9
            # The head's own exits stay shut: no ward- or site-ending events
            # (VTOL_Follow 0x208, VTOL_SeekGuard and VTOL_LandIfCan 0x200, the
            # builders 0x2 / 0x8), +0x66 clear, the ward, site or target live.
            events = rng.choice((1, 0x100, 0x400, 0x500, 0x10, 0x1000))
            if name in ('VTOL_MobileBuild', 'VTOL_HelpBuild', 'VTOL_RepairUnit'):
                events &= ~0xa
            target = ward
            if name == 'VTOL_MobileBuild' and stage <= 2 and rng.random() < 0.5:
                target = 0
            put(unit + 0x12a, 'b', traffic)
            put(unit + 0x78, 'h', foot)
            put(game + 0x19f44, 'I', tick)
            put(mission + 5, 'B', stage)
            put(mission + 6, 'I', 0)
            put(mission + 0xa, 'I', deadline)
            put(mission + 0x16, 'I', target)
            put(mission + 0x56, 'I', 0)
            put(mission + 0x5a, 'I', flags)
            put(mission + 0x66, 'I', 0)
            put(mission + 0x6a, 'I', pending)
            put(block + 4, 'B', 0)
            put(RNG, 'I', seed)
            log.clear()
            draws[0] = 0
            went_on[0] = False
            result, error = p.call(handler, (unit, mission, events))
            if error:
                raise AssertionError((name, i, error))
            names = [m[0] for m in log.get('mission', [])]
            if any(n not in STEP_OUT_KINDS for n in names) or len(names) > 1:
                raise AssertionError((name, i, 'unexpected missions', names))
            stops = sum(1 for f in log.get('flag', []) if f == (8, 0))
            if len(log.get('flag', [])) != stops:
                raise AssertionError((name, i, 'unexpected 51e4d0', log['flag']))
            pushes = len(log.get('push', []))
            if pushes != (1 if names and answer['built'] else 0):
                raise AssertionError((name, i, 'push without a built step-out'))
            expected.append((-1 if went_on[0] else s32(result), get(mission + 5, 'B'), get(mission + 6),
                             get(mission + 0xa), get(mission + 0x6a), get(mission + 0x5a), len(names),
                             STEP_OUT_KINDS[names[0]] if names else 0, log['mission'][0][1] if names else 0, stops,
                             get(RNG), draws[0]))
            fixtures.append('%d %d %d %d %d %d %d %d %d %d' % (index, stage, tick, deadline, pending, flags, traffic,
                                                                foot, seed, int(answer['built'])))
            key = (name, 'step-out' if names else 'none')
            tally[key] = tally.get(key, 0) + 1
    compare('traffic check', fixtures, expected, run(binary, '--traffic-check', fixtures))
    print('PASS: the air-traffic check of %d missions, %d calls (%s)'
          % (len(TRAFFIC_MISSIONS), len(fixtures),
             ', '.join('%s %d/%d' % (n, tally.get((n, 'step-out'), 0), tally.get((n, 'step-out'), 0) +
                                     tally.get((n, 'none'), 0)) for n, _, _ in TRAFFIC_MISSIONS)))


def check_seek_guard(binary):
    """VTOL_SeekGuard (4197d0) against retailVtolSeekGuard on random states of
    both stages and the head: event 0x200, the off-map return, the air-traffic
    check at every wake, the group check (4199b2: 51d1e0(unit, 1, 4) answered
    in or out of the slot; flagged and out, rand(4) == 0 lets the member go,
    50c480 and 4d6a50(unit, 0); else VTOL_Move_Formation at level -3) and the
    flag's rand(10), stage 0's point and angle, stage
    1's search (509ac0, answered), the controller's release, the guard (4de530
    action 7, answered; the mission built or not), the orbit step, the orbit
    point (armed: the selected slot's range + 160; else 160), its radius and
    the sleep."""
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mission, kind, weapon, offmap, controller, block, listed = [HEAP + i * 0x1000 for i in range(8)]
    game = HEAP + 0x20000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    put(game + 0x19f30, 'I', offmap)
    log = {}
    answer = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        size = get(args)
        if size == 0x36:
            return 0, controller
        if size == 0x72:
            return 0, block
        raise AssertionError('unexpected allocation of %#x bytes' % size)

    def point_controller(uc, args):    # 4e40e0(mission, point)
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 4), 12)))
        put(controller + 0xa, 'h', 0)
        record('controller')
        return 2, controller

    def radius(uc, args):
        put(controller + 0xa, 'H', get(args) & 0xffff)
        return 1, 0

    def install(uc, args):             # 4d4d40(controller): 0 lets the mission's controller go
        record('install' if get(args) else 'release')
        put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    def lookup(uc, args):
        record('lookup', bytes(uc.mem_read(get(args), 40)).split(b'\0')[0].decode())
        slot = uc.reg_read(UC_X86_REG_ECX)
        uc.mem_write(slot, b'\x2a')
        return 1, slot

    def construct(uc, args):           # 4d6c40(row, target, point, +0x4e, ...)
        record('mission', (get(args) & 0xff, get(args + 4), get(args + 12, 'i')))
        # The step-out is always built (the port's stepOut() is); the guard
        # as the fixture says.
        put(block + 4, 'B', 1 if answer['built'] or get(args) & 0xff == 0x2a else 0)
        return 12, block

    def search(uc, args):              # 509ac0(position, radius << 16, functor): the functor's vector
        if get(args) != unit + 0x68 or get(args + 4) != (answer['sight'] << 16) & 0xffffffff:
            raise AssertionError('unexpected 509ac0 arguments')
        record('search')
        vector = get(get(args + 8) + 8)
        n = answer['found']
        for k in range(n):
            put(listed + 4 * k, 'I', HEAP + 0x30000 + 0x200 * k)
        put(vector + 4, 'I', listed if n else 0)
        put(vector + 8, 'I', listed + 4 * n if n else 0)
        return 3, 0

    def resolve(uc, args):             # 4de530(&row, action, unit, target, 0, 0)
        if get(args + 4) != 7 or get(args + 8) != unit or get(args + 12) != HEAP + 0x30000:
            raise AssertionError('unexpected 4de530 arguments')
        record('resolve')
        p.uc.mem_write(get(args), bytes([0x33 if answer['guardable'] else 0]))
        return 6, get(args)

    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = point_controller
    p.hooks[0x4e4540] = radius
    p.hooks[0x4d4d40] = install
    p.hooks[0x4d4bf0] = lookup
    p.hooks[0x4d6c40] = construct
    p.hooks[0x4d7750] = lambda uc, a: (record('push'), (2, 0))[1]
    p.hooks[0x4d6da0] = lambda uc, a: (record('destroy'), (0, 0))[1]
    p.hooks[0x4eba00] = lambda uc, a: (record('free'), (0, 0))[1]
    p.hooks[0x42a660] = lambda uc, a: (2, 0)
    p.hooks[0x509ac0] = search
    p.hooks[0x4de530] = resolve
    def group(uc, args):               # 51d1e0(unit, 1, 4)
        if (get(args), get(args + 4), get(args + 8)) != (unit, 1, 4):
            raise AssertionError('unexpected 51d1e0 arguments')
        record('group')
        return 3, 1 if answer.get('out') else 0

    def cancel(uc, args):              # 4d6a50(unit, 0)
        if (get(args), get(args + 4)) != (unit, 0):
            raise AssertionError('unexpected 4d6a50 arguments')
        record('cancel')
        return 2, 0

    p.hooks[0x51d1e0] = group
    p.hooks[0x50c480] = lambda uc, a: (record('leave', get(a)), (1, 0))[1]
    p.hooks[0x4d6a50] = cancel
    p.hooks[0x51e4d0] = lambda uc, a: (_ for _ in ()).throw(AssertionError('unexpected 51e4d0'))
    p.freeze_hooks()
    watch = emurange.Watch(p, ((0x4197d0, 0x419daf),) + emurange.HELPERS)
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1), begin=0x535cc0, end=0x535cc0)

    put(unit + 0xb4, 'I', kind)
    put(unit + 0xc, 'I', weapon)
    rng = random.Random(0x4197d0)
    outcomes = {}
    fixtures, expected, left = [], [], []
    stages = {}
    for i in range(8000):
        stage = rng.choice((0, 0, 1, 1, 1, 1, 2))
        events = rng.choice((1, 1, 1, 0x100, 0x400, 0x500, 0x10, 0x8, 0x200, 0x700))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, rng.randrange(2**32)))
        flags = rng.choice((0x1000400, 0x1000600, 0x9000400, 0x8000000))
        width = rng.choice((4096 - 32, 8192 - 32, rng.randrange(256, 12000)))
        height = rng.choice((4096 - 128, 8192 - 128, rng.randrange(256, 12000)))
        on_map = rng.random() < 0.85
        flies = rng.random() < 0.95
        armed = rng.random() < 0.6
        x = rng.randrange(-300, width + 300) * 65536 + rng.randrange(65536)
        z = rng.randrange(-300, height + 300) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536 + rng.randrange(65536)
        point = (s32(rng.randrange(2**32)), s32(rng.randrange(2**32)), s32(rng.randrange(2**32)))
        if i % 4 == 0:
            point = (0, 0, 0)
        angle = rng.randrange(2**32)
        weapon_range = rng.choice((0, 64, 200, 320, rng.randrange(0, 900)))
        traffic = rng.choice((0, 2, 4, 5, 6, 9, 12, -3))
        foot = rng.choice((1, 2, 3, 4))
        seed = rng.randrange(2**32)
        answer.update(found=rng.choice((0, 0, 0, 1, 3)), guardable=rng.random() < 0.8, built=rng.random() < 0.9,
                      sight=rng.choice((200, 256, 320, 400)), out=rng.random() < 0.2)
        mover = 1 if flies or rng.random() < 0.5 else 0
        put(unit + 8, 'I', mover)
        put(kind + 0x260, 'I', 0x800 if (flies or not mover) else 0)
        put(kind + 0x226, 'h', answer['sight'])
        flies_port = bool(mover) and bool(get(kind + 0x260) & 0x800)
        put(unit + 0x130, 'I', 0x1000000 | (0x8000000 if armed else 0))
        put(weapon + 0x90, 'i', weapon_range)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0x78, 'h', foot)
        put(unit + 0xa4, 'I', offmap + 4 if on_map else offmap)
        put(unit + 0x12a, 'b', traffic)
        put(game + 0x19f44, 'I', tick)
        put(game + 0x19e90, 'I', width)
        put(game + 0x19e94, 'I', height)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x22, 'iii', *point)
        put(mission + 0x4e, 'I', angle)
        put(mission + 0x5a, 'I', flags)
        put(mission + 0x6a, 'I', pending)
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        put(block + 4, 'B', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        result, error = p.call(0x4197d0, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        left.append(watch.left())
        missions = log.get('mission', [])
        names = log.get('lookup', [])
        # A step-out's D (+0x4e) is (rand(15) + footX) x 16, never 0; the
        # formation's is 0.
        steps = [m for m in missions if m[0] == 0x2a and m[1] == 0 and m[2]]
        formations = [m for m in missions if m[0] == 0x2a and m[1] == 0 and not m[2]]
        guards = [m for m in missions if m[0] == 0x33]
        if len(steps) + len(guards) + len(formations) != len(missions):
            raise AssertionError((i, 'unexpected missions', missions))
        if guards and guards[0][1] != HEAP + 0x30000:
            raise AssertionError((i, 'guard for another unit'))
        if sorted(names) != sorted(['VTOL_StepOut'] * len(steps) + ['VTOL_Move_Formation'] * len(formations)):
            raise AssertionError((i, 'unexpected lookups', names))
        if log.get('leave') and log['leave'] != [unit]:
            raise AssertionError((i, 'leave', log['leave']))
        if log.get('group'):
            key = ('formation' if formations else 'leave' if log.get('cancel') else '?') if answer['out'] \
                else 'in slot'
            if key == '?':
                raise AssertionError((i, 'out of the slot without a formation or a leave'))
            outcomes[key] = outcomes.get(key, 0) + 1
        installs = len(log.get('install', []))
        goal = struct.unpack('<3i', p.uc.mem_read(controller + 0x26, 12)) if installs else (0, 0, 0)
        guarded = len(guards) if answer['built'] else 0
        expected.append((s32(result), get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa),
                         get(mission + 0x6a), get(mission + 0x5a), get(mission + 0x4e) & 0xffff,
                         *struct.unpack('<3i', p.uc.mem_read(mission + 0x22, 12)), installs, *goal,
                         get(controller + 0xa, 'h') if installs else 0, len(log.get('search', [])),
                         len(log.get('release', [])), len(log.get('resolve', [])), len(steps),
                         1 if steps else 0, 0, steps[-1][2] if steps else 0, get(RNG), draws[0],
                         len(log.get('group', [])), len(log.get('cancel', [])), len(formations)))
        if len(log.get('cancel', [])) != len(log.get('leave', [])):
            raise AssertionError((i, '4d6a50 without 50c480'))
        guard_ok = int(answer['guardable'] and answer['built'])
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, angle & 0xffff, *point,
                                           s32(x), s32(y), s32(z), weapon_range, int(armed), int(flies_port),
                                           int(on_map), width, height, seed, int(answer['found'] > 0), guard_ok,
                                           traffic, foot, int(answer['out'])))))
        key = 'off' if not on_map else stage
        stages[key] = stages.get(key, 0) + 1
    rows = run(binary, '--air-seek-guard', fixtures)
    # The port counts a guard() call per offer; native offers one (resolve)
    # and builds it only for a row: the port's guardCalls equals resolves.
    compare('seek guard', fixtures, expected, rows, left, (11, 13))

    def tally(column, value=None):
        return sum(1 for row in expected if (row[column] == value if value is not None else row[column]))
    print('PASS: VTOL_SeekGuard, %d calls (%s), every state, point, controller, search, guard, push and draw '
          '(%d orbit points, %d searches, %d guards offered, %d put ahead, %d step-outs, %d ended (8)); group check %s'
          % (len(fixtures), ' '.join('%s:%d' % kv for kv in sorted(stages.items(), key=str)), tally(10), tally(15),
             tally(17), tally(0, 3), tally(18), tally(0, 8), ', '.join('%s %d' % kv for kv in sorted(outcomes.items()))))


def check_air_strike(binary):
    """AirStrike (41b5e0) against retailAirStrike on random states of every
    stage and the head: the lost-target exit (events 0x2a, a dead, dying,
    carried or unplaced target, a selected weapon without `dropped`) and its
    VTOL_SEEKATTACK, the lost-target seek of a strike without a target, event
    0x40, the point, the leash, the flag's rand(25); stage 0's take-off, stage
    1's slot calls and close pass, stage 2's half-way point, stage 4's
    air-traffic check (the rescan, D, the push) and release ring (the x87
    sequence over cruisealt, gravity and the mover's +0x22, and the controller
    that follows the target), stage 5's aim and run, stage 6's clear and turn.
    The slot calls, the scan and attack (4d8370, 4d7de0), the controllers and
    the mission constructors and pushes are stubs that answer as the fixture
    says and record their arguments; 519ac0, 51b340, 535cc0, the sines, _ftol
    and the sleep are native."""
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mission, kind, weapon, target, mover, controller, block, found = [HEAP + i * 0x1000 for i in range(9)]
    game = HEAP + 0x20000

    def put(a, fmt, *v):
        p.uc.mem_write(a, struct.pack('<' + fmt, *v))

    def get(a, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]

    put(0x62d55c, 'I', game)
    log = {}
    answer = {}

    def record(name, value=None):
        log.setdefault(name, []).append(value)

    def allocate(uc, args):
        size = get(args)
        if size == 0x36:
            return 0, controller
        if size == 0x72:
            return 0, block
        raise AssertionError('unexpected allocation of %#x bytes' % size)

    def point_controller(uc, args):    # 4e40e0(mission, point)
        p.uc.mem_write(controller + 0x26, bytes(uc.mem_read(get(args + 4), 12)))
        put(controller + 0xa, 'h', 0)
        record('point')
        return 2, controller

    def follow_controller(uc, args):   # 4e3f70(mission, target)
        if get(args + 4) != target:
            raise AssertionError('follow controller on another unit')
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        record('follow')
        return 2, controller

    def radius(uc, args):
        put(controller + 0xa, 'H', get(args) & 0xffff)
        return 1, 0

    def install(uc, args):
        record('install', get(args))
        put(mission + 0x6a, 'I', get(mission + 0x6a) & 0xffffc8ff)
        return 1, 0

    def lookup(uc, args):
        record('lookup', bytes(uc.mem_read(get(args), 40)).split(b'\0')[0].decode())
        slot = uc.reg_read(UC_X86_REG_ECX)
        uc.mem_write(slot, b'\x2a')
        return 1, slot

    def construct(uc, args):           # 4d6c40(row, target, point, +0x4e, ...)
        name = log['lookup'][-1]
        at = struct.unpack('<3i', uc.mem_read(get(args + 8), 12)) if get(args + 8) else (0, 0, 0)
        record('mission', (name, get(args + 4), at, get(args + 12, 'i')))
        put(block + 4, 'B', 1 if name != 'VTOL_StepOut' or answer['step'] else 0)
        return 12, block

    def aim_ground(uc, args):          # 51a770(unit, point, slot)
        if get(args) != unit or get(args + 8) & 0xff != 0:
            raise AssertionError('unexpected 51a770 arguments')
        record('aim', struct.unpack('<3i', uc.mem_read(get(args + 4), 12)))
        return 3, 0

    def scan(uc, args):
        record('scan')
        return 1, (found if answer['found'] == 1 else target if answer['found'] == 2 else 0)

    def attack(uc, args):
        if (get(args), get(args + 4), get(args + 8), get(args + 12), get(args + 16), get(args + 20)) != \
                (unit, found, 0, 0, 1, 0):
            raise AssertionError('unexpected 4d7de0 arguments')
        record('attack')
        return 6, 1 if answer['attack'] else 0

    p.hooks[0x4eb9e0] = allocate
    p.hooks[0x4e40e0] = point_controller
    p.hooks[0x4e3f70] = follow_controller
    p.hooks[0x4e4540] = radius
    p.hooks[0x4d4d40] = install
    p.hooks[0x4d4bf0] = lookup
    p.hooks[0x4d6c40] = construct
    p.hooks[0x4d7750] = lambda uc, a: (record('push'), (2, 0))[1]
    p.hooks[0x4d77b0] = lambda uc, a: (record('insert'), (2, 0))[1]
    p.hooks[0x4d6da0] = lambda uc, a: (record('destroy'), (0, 0))[1]
    p.hooks[0x4eba00] = lambda uc, a: (record('free'), (0, 0))[1]
    p.hooks[0x416c50] = lambda uc, a: (record('begin'), (4, 0))[1]
    p.hooks[0x519b10] = lambda uc, a: (record('auto', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x519b70] = lambda uc, a: (record('manual', get(a) & 0xff), (1, 0))[1]
    p.hooks[0x51a770] = aim_ground
    p.hooks[0x51a7f0] = lambda uc, a: (record('clear', (get(a), get(a + 4) & 0xff)), (2, 0))[1]
    p.hooks[0x4d8370] = scan
    p.hooks[0x4d7de0] = attack
    def group(uc, args):               # 51d1e0(unit, 1, 5)
        if (get(args), get(args + 4), get(args + 8)) != (unit, 1, 5):
            raise AssertionError('unexpected 51d1e0 arguments')
        record('group')
        return 3, 1 if answer['out'] else 0

    p.hooks[0x51d1e0] = group
    p.hooks[0x50c480] = lambda uc, a: (record('leave', get(a)), (1, 0))[1]
    p.freeze_hooks()
    watch = emurange.Watch(p, ((0x41b5e0, 0x41bf7f),) + emurange.HELPERS)
    draws = [0]
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: draws.__setitem__(0, draws[0] + 1), begin=0x535cc0, end=0x535cc0)

    # A non-switching flyer with its weapon in slot 0 (519ac0 answers 0).
    put(unit + 0xb4, 'I', kind)
    put(unit + 0xc, 'I', weapon)
    put(unit + 8, 'I', mover)
    put(kind + 0x1aa, 'I', weapon)
    rng = random.Random(0x41b5e0)
    fixtures, expected, left = [], [], []
    stages = {}
    for i in range(12000):
        stage = rng.choice((0, 1, 1, 2, 2, 3, 4, 4, 4, 4, 5, 5, 6, 6, 7))
        events = rng.choice((1, 1, 1, 1, 1, 0x100, 0x400, 0x500, 0x700, 0x101, 0x40, 0x2, 0x8, 0x20, 0x41, 0x1000))
        tick = rng.randrange(1000000)
        deadline = rng.choice((0xffffffff, rng.randrange(2**32)))
        pending = rng.choice((0, 0x100, 0x500, 0x3700, rng.randrange(2**32)))
        flags = rng.choice((0x1000600, 0x1000400, 0x1000400, 0x9000600, 0x8000400, 0x1000200))
        x = rng.randrange(0, 8000) * 65536 + rng.randrange(65536)
        z = rng.randrange(0, 8000) * 65536 + rng.randrange(65536)
        y = rng.randrange(0, 512) * 65536 + rng.randrange(65536)
        spread = rng.choice((0, 16, 200, 470, 490, 900, 3000))
        tx = x + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        tz = z + rng.randrange(-spread, spread + 1) * 65536 + rng.randrange(65536)
        ty = rng.randrange(0, 256) * 65536
        point = (s32(rng.randrange(2**32)), s32(rng.randrange(2**32)), s32(rng.randrange(2**32)))
        if i % 3 == 0:
            point = (s32(tx), s32(ty), s32(tz))
        heading = rng.randrange(65536)
        armed = rng.random() < 0.95
        dropped = rng.random() < 0.97
        flies = rng.random() < 0.95
        unit_live = rng.random() < 0.9
        dying = rng.random() < 0.05
        fire = rng.choice((0x40000, 0x80000, 0xc0000, 0))
        has_target = rng.random() < 0.7
        t_live = rng.random() < 0.92
        t_dying = rng.random() < 0.05
        carried = rng.random() < 0.05
        placed = rng.random() < 0.95
        last = rng.random() < 0.8
        automatic = rng.random() < 0.5
        leash = rng.choice((0, 0, 0, 0, 0, 0, 100, 400, rng.randrange(0, 3000)))
        ox, oz = rng.randrange(-2000, 10000), rng.randrange(-2000, 10000)
        if rng.random() < 0.6:   # a leash round the flyer itself, its boundary within reach
            ox, oz = x // 65536 + rng.randrange(-450, 451), z // 65536 + rng.randrange(-450, 451)
        run_length = rng.choice((0, 64, 128, 300, rng.randrange(0, 1000)))
        cruise = rng.choice((0, 50, 100, 150, rng.randrange(-50, 400)))
        gravity = rng.choice((0x1fdb, 0x1fdb, 0x2000, 0x4000, rng.randrange(1, 0x10000)))
        speed = rng.choice((0, 3, 6, 10, rng.randrange(-20, 400)))
        traffic = rng.choice((0, 2, 4, 5, 6, 9, 12, -3))
        foot = rng.choice((1, 2, 3, 4))
        seed = rng.randrange(2**32)
        answer.update(found=rng.choice((0, 1, 2)), attack=rng.random() < 0.5, step=rng.random() < 0.9,
                      out=rng.random() < 0.25)
        nxt = 0 if last else rng.choice((0, 0x3000400, 0x1000400, 0x5012000, 0x15000400))
        put(unit + 0x130, 'I', (0x8000000 if armed else 0) | (0x1000000 if unit_live else 0) | (0x1000 if dying else 0) |
            fire)
        put(kind + 0x260, 'I', 0x800 if flies else 0)
        put(weapon + 0xc8, 'I', 0x20 if dropped else 0)
        put(unit + 0x68, 'iii', s32(x), s32(y), s32(z))
        put(unit + 0x7e, 'H', heading)
        put(unit + 0x12a, 'b', traffic)
        put(unit + 0x78, 'h', foot)
        put(kind + 0x234, 'H', run_length)
        put(kind + 0x23a, 'h', cruise)
        put(game + 0x19ecc, 'i', gravity)
        put(mover + 0x22, 'h', speed)
        put(target + 0x68, 'iii', s32(tx), s32(ty), s32(tz))
        put(target + 0x130, 'I', (0x1000000 if t_live else 0) | (0x1000 if t_dying else 0) | (1 if placed else 0))
        put(target + 0x114, 'B', 4 if carried else 0)
        put(game + 0x19f44, 'I', tick)
        put(mission + 5, 'B', stage)
        put(mission + 6, 'I', 0)
        put(mission + 0xa, 'I', deadline)
        put(mission + 0x16, 'I', target if has_target else 0)
        put(mission + 0x22, 'iii', *point)
        put(mission + 0x2e, 'hh', ox if ox < 32768 else ox - 65536, oz if oz < 32768 else oz - 65536)
        put(mission + 0x52, 'I', 1 if automatic else 0)
        put(mission + 0x56, 'i', leash)
        put(mission + 0x5a, 'I', flags)
        put(mission + 0x66, 'I', 0 if last else block + 0x40)
        put(block + 0x40 + 0x5a, 'I', nxt)     # past the 0x72-byte block a push takes
        put(mission + 0x6a, 'I', pending)
        put(controller + 0x26, 'iii', 0, 0, 0)
        put(controller + 0xa, 'h', 0)
        put(RNG, 'I', seed)
        log.clear()
        draws[0] = 0
        result, error = p.call(0x41b5e0, (unit, mission, events))
        if error:
            raise AssertionError((i, error))
        left.append(watch.left())
        missions = log.get('mission', [])
        steps = [m for m in missions if m[0] == 'VTOL_StepOut']
        seeks = [m for m in missions if m[0] == 'VTOL_SEEKATTACK']
        formations = [m for m in missions if m[0] == 'VTOL_Move_Formation']
        if len(steps) + len(seeks) + len(formations) != len(missions):
            raise AssertionError((i, 'unexpected missions', missions))
        if any(m[1] not in (0, target) for m in seeks):
            raise AssertionError((i, 'seek for another unit', seeks))
        installs = len(log.get('install', []))
        follows = len(log.get('follow', []))
        goal = struct.unpack('<3i', p.uc.mem_read(controller + 0x26, 12)) if installs and not follows else (0, 0, 0)
        aims = log.get('aim', [])
        if log.get('auto') and any(a != 3 for a in log['auto']):
            raise AssertionError((i, 'auto slots', log['auto']))
        if log.get('manual') and any(a != 0 for a in log['manual']):
            raise AssertionError((i, 'manual slot', log['manual']))
        expected.append((s32(result), get(mission + 5, 'B'), get(mission + 6), get(mission + 0xa), get(mission + 0x6a),
                         get(mission + 0x5a), *struct.unpack('<3i', p.uc.mem_read(mission + 0x22, 12)),
                         installs - follows, follows, *goal, get(controller + 0xa, 'h') if installs else 0,
                         len(log.get('begin', [])), len(log.get('auto', [])), len(log.get('manual', [])), len(aims),
                         aims[-1][0] if aims else 0, aims[-1][2] if aims else 0, len(log.get('clear', [])),
                         len(log.get('scan', [])), len(steps), len(seeks), steps[-1][3] if steps else 0,
                         int(bool(seeks and seeks[-1][1])), get(RNG), *(seeks[-1][2] if seeks else (0, 0, 0)),
                         draws[0], len(log.get('group', [])), len(log.get('leave', [])), len(formations)))
        if log.get('leave') and log['leave'] != [unit]:
            raise AssertionError((i, 'leave', log['leave']))
        # The port's retarget() is the scan, the not-the-target test and the
        # attack together: it answers yes only when all three do.
        retargets = int(answer['found'] == 1 and answer['attack'])
        fixtures.append(' '.join(map(str, (stage, events, tick, deadline, pending, flags, *point, s32(x), s32(y),
                                           s32(z), s32(tx), s32(ty), s32(tz), heading, int(armed), int(dropped),
                                           int(flies), int(unit_live and not dying), int(fire == 0),
                                           int(has_target), int(t_live and not t_dying), int(carried), int(placed),
                                           int(last), leash, ox if ox < 32768 else ox - 65536,
                                           oz if oz < 32768 else oz - 65536, run_length, cruise, gravity, speed,
                                           traffic, foot, int(automatic), seed, retargets, int(answer['step']),
                                           int(answer['out']), nxt))))
        stages[stage] = stages.get(stage, 0) + 1
    rows = run(binary, '--air-strike', fixtures)
    # The port counts a retarget() call per offer; native scans once and
    # attacks only a found unit that is not the target.
    actual = []
    for r in rows:
        r = list(r)
        actual.append(tuple(r))
    # The mission's point is any 32-bit value. 32768 px or more from the flyer on an axis, their difference does
    # not fit 32 bits: retail's wraps (the watch sees it, in that call), the port's keeps its sign (emurange.py),
    # and the distance, the heading and the point built from them differ. Every other row must match exactly.
    past = 0
    for i, (got, want) in enumerate(zip(actual, expected)):
        # retargetCalls (port) against scans (native): the same offers.
        if got != want:
            if left[i]:
                past += 1
                continue
            raise AssertionError('air strike fixture %d: %s\n  port   %s\n  native %s' % (i, fixtures[i], got, want))
    print('air strike: retail leaves the range in %d fixtures (a difference wraps); the port differs in %d of '
          'them (ours)' % (sum(map(bool, left)), past))
    if len(actual) != len(expected):
        raise AssertionError('air strike: row count %d, expected %d' % (len(actual), len(expected)))

    def tally(column, value=None):
        return sum(1 for row in expected if (row[column] == value if value is not None else row[column]))
    print('PASS: AirStrike, %d calls (%s), every state, point, controller, slot call, push and draw '
          '(%d point controllers, %d following controllers, %d aims, %d step-outs, %d seeks, %d ended (5), '
          '%d group checks, %d leaves, %d formations)'
          % (len(fixtures), ' '.join('%s:%d' % kv for kv in sorted(stages.items())), tally(9), tally(10),
             tally(18), tally(23), tally(24), tally(0, 5), tally(32), tally(33), tally(34)))


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-dbg/retail_motion_test'
    check_counter(binary)
    check_step_out(binary)
    check_hover(binary)
    check_mission_checks(binary)
    check_seek_guard(binary)
    check_air_strike(binary)


if __name__ == '__main__':
    main()
