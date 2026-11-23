#!/usr/bin/env python3
"""Run retail's ground attack (Attack_Chase) end to end for one unit and watch
its walk controller race its aim.

Observation only: the local retail binary (assets/game) and shipped COB/FBI
files (assets/extracted/all) are read at run time; nothing is copied.

One VERMAGE (Kirenna) on a synthetic flat map attacks a passive entity along
+x. Every tick runs, natively and in 51d3e0's per-unit order:
    52ae90  weapon update (52fe30 aim start + facing request, 52fff0, 530140)
    56c870  the unit's COB VM: shipped vermage.cob in retail's VM, its GET and
            SET through the real host (50ceb0, 50d450)
    4d8450  mission dispatch -> 403bb0 Attack_Chase (stages 0, 2 and 1)
    4dc800  the mover: ground navigator (5f2a24) with the mission's circle
            controller (4d4da0 -> 4e2500), braking, the idle pivot, and the
            TurnDirection/MoveRate call-ins (56c640) into the same VM
Controlled inputs: the aim angles (52fd80, fixed), the led point (51aa50, the
target's origin), the damage lookup (531e50), the group check (51d1e0, none),
the retarget scan (4d8370, none), the CRT reload roll (5d4444), and the
display/projectile sinks. The navigator's search never runs, so it steers its
straight two-point route, as a search would on open ground.

    probe_attack_chase_stop.py                 one engagement, timeline
    probe_attack_chase_stop.py --grid          36 order ticks x 4 distances,
                                               arrival forced in range
                                               (--approach-shrink 24)
    probe_attack_chase_stop.py --distances 620 760   the plain approach

--sweep N models the fire-at-will sweep (40f330, after the unit updates)
filling the auto slot every N ticks once the target is in reach.

--switch-tick T gives the unit a second slot (weaponswitching, a copy of the
first weapon) and switches to it natively (51a8b0, which raises 0x40 at
+0xd0) at tick T right after the weapon update 52ae90, as the mana fallback
does (52af6c..52afa1). It prints the tick 51a700 first gave slot 1 the
target: Attack_Chase waits in its fire stage on 0x1f40068 (404493), so the
dispatch of the same tick wakes on 0x40 and aims the new slot (40448e). With
--no-event the 0x40 is taken back before the dispatch: the slot waits for
the stage's own wake (rand(6) + 3 ticks).
"""
import argparse
import collections
import concurrent.futures
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import HEAP  # noqa: E402
from emuphase import ARENA, GS, TYPE, Phase  # noqa: E402
from check_movement_callback_order import SCRIPT_ROOT, fbi_info, profile_type_fields  # noqa: E402
from check_native_mover_cob_join import names_at, u32  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import UC_X86_REG_ESP  # noqa: E402

ENTITIES = ARENA + 0x2000000      # 51a9a0's entity array, stride 0x138
TARGET_TYPE = ARENA + 0x2100000
WEAPON = ARENA + 0x2200000
MISSION = ARENA + 0x2300000
DESCRIPTORS = ARENA + 0x2400000
PLAYER = ARENA + 0x0E00000        # emuphase's player object
ATTACK_CHASE = 0x5EB8D1           # mission registry row


def put(uc, a, v, fmt='I'):
    uc.mem_write(a, struct.pack('<' + fmt, v & 0xffffffff if fmt in 'Ii' else v))


def s32(uc, a):
    return struct.unpack('<i', uc.mem_read(a, 4))[0]


def h16(uc, a):
    return struct.unpack('<H', uc.mem_read(a, 2))[0]


def engagement(unit_name='vermage', distance=664, offset=0, weapon_range=400, tolerance=1024, heading=0xC000,
               ticks=400, order_tick=5, fire_state=0, auto=False, sweep=0, sweep_phase=0, approach_shrink=0,
               reload_ticks=120, switch_tick=0, keep_event=True):
    info = fbi_info(unit_name)
    p = Phase(160, 160)
    uc = p.uc
    unit = p.unit(10, 10)
    mover, navigator = unit + 0x300, ARENA + 0x0D10000
    settings, options = ARENA + 0x0D21000, ARENA + 0x0D20000
    put(uc, 0x62D55C, GS)
    put(uc, 0x62D558, settings)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    uc.mem_write(GS + 0x19EF8, bytes((0,)))
    stride = 32
    sectors = p._alloc(stride * stride * 10)
    put(uc, GS + 0x19F18, sectors)
    put(uc, GS + 0x19F1C, stride)
    put(uc, GS + 0x19F30, 1)
    for index in range(stride * stride):
        uc.mem_write(sectors + index * 10 + 1, b'\x64')
    sx, sz, y = 400, 1280, 120
    current_sector = sectors + ((sz >> 7) * stride + (sx >> 7)) * 10
    put(uc, unit + 0xA4, current_sector)
    put(uc, current_sector + 6, unit)
    uc.mem_write(unit + 0x68, struct.pack('<3i', sx << 16, y << 16, sz << 16))
    uc.mem_write(unit + 0x7E, struct.pack('<H', heading & 0xffff))   # 0xc000 faces +x
    put(uc, unit + 0x12B, int(float(info.get('maxvelocity', 1.25)) * 65536))
    # Alive, armed, a surface body; fire state in bits 18-19.
    put(uc, unit + 0x130, 0x01000000 | 0x08000000 | 1 | (fire_state << 18))
    put(uc, unit + 0xD8, 1000.0, 'f')
    put(uc, unit + 0xF4, 1.0, 'f')
    put(uc, mover, navigator)
    put(uc, mover + 0x30, 0x7FFFFFFF)
    uc.mem_write(mover + 0x36, struct.pack('<H', 1))
    uc.mem_write(navigator, bytes(0x200))
    put(uc, navigator, 0x5F2A24)
    put(uc, navigator + 8, unit)
    put(uc, GS + 0x19E70, p._alloc(0x400))       # an idle search scheduler
    uc.mem_write(unit + 0x78, struct.pack('<hh', 2, 2))
    uc.mem_write(unit + 0x74, struct.pack('<hh', (sx - 16) // 16, (sz - 16) // 16))
    profile_type_fields(uc, info, dict(model_top=0, waterline=int(float(info.get('waterline', 0)))))
    put(uc, TYPE + 0x260, 0x10000 if switch_tick or float(info.get('weaponswitching', 0)) else 0)
    put(uc, TYPE + 0x1AA, WEAPON)
    put(uc, TYPE + 0x1AE, WEAPON + 0x200 if switch_tick else 0)
    put(uc, TYPE + 0x1B2, 0)
    put(uc, unit + 0xB8, PLAYER)
    # Slot 0's WeaponType: a LineOfSightWeapon (vtable 5f39f4) with itself as handler.
    uc.mem_write(WEAPON, bytes(0x200))
    put(uc, WEAPON, 0x5F39F4)
    put(uc, WEAPON + 0x40, WEAPON)
    put(uc, WEAPON + 0x90, weapon_range)
    uc.mem_write(WEAPON + 0x9C, struct.pack('<H', reload_ticks))
    uc.mem_write(WEAPON + 0xC0, struct.pack('<H', tolerance))
    record = unit + 12
    put(uc, record, WEAPON)
    uc.mem_write(record + 4, struct.pack('<HH', 0, 0x8000))       # no target
    uc.mem_write(record + 0x1A, struct.pack('<H', 4 if auto else 0))
    if switch_tick:
        # Slot 1: the same weapon (its own WeaponType), no target, no reload.
        uc.mem_write(WEAPON + 0x200, bytes(uc.mem_read(WEAPON, 0x200)))
        put(uc, WEAPON + 0x200 + 0x40, WEAPON + 0x200)
        put(uc, record + 28, WEAPON + 0x200)
        uc.mem_write(record + 28 + 4, struct.pack('<HH', 0, 0x8000))
        uc.mem_write(record + 28 + 0x1A, struct.pack('<H', 4 if auto else 0))
    # The target: entity 2 of 51a9a0's array.
    target = ENTITIES + 2 * 0x138
    uc.mem_write(ENTITIES, bytes(0x138 * 8))
    put(uc, GS + 0x14E84, ENTITIES)
    put(uc, GS + 0x14E88, ENTITIES + 0x138 * 7)
    uc.mem_write(target + 2, struct.pack('<H', 2))
    tx, tz = sx + distance, sz + offset
    uc.mem_write(target + 0x68, struct.pack('<3i', tx << 16, y << 16, tz << 16))
    uc.mem_write(target + 0x74, struct.pack('<hhhh', tx // 16, tz // 16, 1, 1))
    put(uc, target + 0x130, 0x01000000 | 1)
    put(uc, target + 0xB4, TARGET_TYPE)
    put(uc, target + 0xB8, PLAYER + 0x200)
    uc.mem_write(TARGET_TYPE, bytes(0x300))
    # Attack_Chase as descriptor 0 of the runtime mission table.
    row = bytes(uc.mem_read(ATTACK_CHASE, 25))
    uc.mem_write(DESCRIPTORS, row)
    put(uc, 0x62DB84, DESCRIPTORS)
    put(uc, 0x62DB88, DESCRIPTORS + 25)
    uc.mem_write(MISSION, bytes(0x100))
    put(uc, MISSION + 0x0E, unit)
    put(uc, MISSION + 0x16, target)
    uc.mem_write(MISSION + 0x12, struct.pack('<HH', 2, 0x8000))
    put(uc, MISSION + 0x5A, struct.unpack_from('<I', row, 0x11)[0])

    # The unit's COB VM, as the native mover/COB joins attach it, with the real host.
    cob = (SCRIPT_ROOT / f'{unit_name}.cob').read_bytes()
    _, script_count, piece_count, code_words, static_count, _, entry_off, names_off, _, code_off = \
        struct.unpack_from('<10I', cob)
    script_names = names_at(cob, names_off, script_count)
    vm, desc, statics, pieces, vtable, scratch = (HEAP + o for o in (0, 0x10000, 0x40000, 0x50000, 0x60000, 0x70000))
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    names_ptrs, names_text, sounds, model = HEAP + 0x90000, HEAP + 0x91000, HEAP + 0x80000, HEAP + 0x98000
    saved = bytes(0xA48 + static_count * 4 + piece_count * 0x6C)
    cursor = 0
    pose = [[0] * 6 for _ in range(piece_count)]

    def read(_uc, sp):
        nonlocal cursor
        dst, size = u32(uc, sp), u32(uc, sp + 4)
        part = saved[cursor:cursor + size]
        cursor += len(part)
        if part:
            uc.mem_write(dst, part)
        return 2, len(part)

    def seek(_uc, sp):
        nonlocal cursor
        cursor = u32(uc, sp)
        return 1, 0

    def write_pose(base):
        def call(_uc, sp):
            piece, axis, value = struct.unpack('<3I', uc.mem_read(sp, 12))
            pose[piece][base + axis] = value
            return 3, 0
        return call

    def read_pose(base):
        def call(_uc, sp):
            piece, axis = struct.unpack('<2I', uc.mem_read(sp, 8))
            return 2, pose[piece][base + axis]
        return call

    host = {0: write_pose(0), 4: write_pose(3), 8: lambda _u, _s: (2, 0), 12: lambda _u, _s: (2, 0),
            16: lambda _u, _s: (2, 0), 20: lambda _u, _s: (2, 0), 24: read_pose(0), 28: read_pose(3),
            44: lambda _u, _s: (2, 0), 48: lambda _u, _s: (2, 0), 52: lambda _u, _s: (2, 0),
            56: lambda _u, sp: (2, u32(uc, sp + 4))}
    for name, value in ((vm, vtable), (vm + 4, 30), (vm + 0x0C, desc), (vm + 0x10, struct.unpack_from('<I', saved)[0]),
                        (vm + 0x14, statics), (vm + 0x18, pieces), (vm + 0xA64, model), (model + 0x0C, unit),
                        (desc + 4, script_count), (desc + 8, piece_count), (desc + 0x10, static_count),
                        (desc + 0x18, entries), (desc + 0x24, code), (desc + 0x2C, sounds)):
        put(uc, name, value)
    uc.mem_write(code, cob[code_off:code_off + code_words * 4])
    uc.mem_write(entries, cob[entry_off:entry_off + script_count * 4])
    pointers, at = [], names_text
    for name in script_names:
        uc.mem_write(at, name.encode('ascii') + b'\0')
        pointers.append(at)
        at += len(name) + 1
    uc.mem_write(names_ptrs, b''.join(struct.pack('<I', a) for a in pointers))
    for offset in range(0, 100, 4):
        put(uc, vtable + offset, 0x56A000 + offset * 4)
    for offset, fn in host.items():
        p.icd.hooks[0x56A000 + offset * 4] = fn
    put(uc, vtable + 80, 0x50D450)   # SET
    put(uc, vtable + 84, 0x50CEB0)   # GET
    p.icd.hooks.update({0x5359A0: lambda _u, _s: (0, len(saved)), 0x5359C0: seek, 0x535A30: read,
                        0x5BA3D0: lambda _u, _s: (0, scratch), 0x5BA5D0: lambda _u, _s: (0, 0)})

    def angles(_uc, sp):
        _, state = struct.unpack('<2I', uc.mem_read(sp, 8))
        uc.mem_write(state + 0x16, struct.pack('<2H', 0, 0))
        return 2, 0

    def led(_uc, sp):
        source, output, slot = struct.unpack('<3I', uc.mem_read(sp, 12))
        index, marker = struct.unpack('<HH', uc.mem_read(source + 12 + slot * 28 + 4, 4))
        if marker != 0x8000 or not index:
            return 3, 0
        uc.mem_write(output, bytes(uc.mem_read(target + 0x68, 12)))
        return 3, 1

    shots = []

    def release(_uc, sp):
        _, state = struct.unpack('<2I', uc.mem_read(sp, 8))
        uc.mem_write(state + 0x1A, struct.pack('<H', h16(uc, state + 0x1A) & 0xFF0F))
        shots.append(now[0])
        return 2, 0

    p.icd.hooks.update({0x52FD80: angles, 0x51AA50: led, 0x530220: release,
                        0x4EA4F0: lambda _u, _s: (4, 0), 0x4EA560: lambda _u, _s: (2, 0),
                        0x531E50: lambda _u, _s: (1, 1500), 0x5D4444: lambda _u, _s: (0, 16384),
                        0x51D1E0: lambda _u, _s: (3, 0), 0x4D8370: lambda _u, _s: (1, 0)})
    if approach_shrink:
        # The mission's approach radius only (52aba0 = slot range), so the
        # circle's cell arrival lands inside the 2D range: no re-approach.
        p.icd.hooks[0x52ABA0] = lambda _u, _s: (2, weapon_range - approach_shrink)
    p.icd.hooks.pop(0x535CC0, None)               # the game RNG runs natively
    p.icd.freeze_hooks()

    now = [0]
    put(uc, GS + 0x19F44, 0)
    put(uc, 0x62D55C, 0)
    result, error = p.icd.call(0x56DC00, (sounds,), ecx=vm)
    if error or result != 1:
        raise RuntimeError(('COB initialization', result, error))
    put(uc, 0x64186C, 1)
    _, error = p.icd.call(0x56C5F0, ([n.lower() for n in script_names].index('create'), 0, 1), ecx=vm)
    if error:
        raise RuntimeError(('Create', error))
    put(uc, desc + 0x1C, names_ptrs)
    put(uc, 0x62D55C, GS)
    put(uc, unit + 0xBC, vm)
    calls = []

    def callin(uc_, _a, _s, _d):
        a = struct.unpack('<8I', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP) + 4, 32))
        name = bytes(uc_.mem_read(a[0], 64)).split(b'\0', 1)[0].decode('ascii')
        calls.append((now[0], name, tuple(struct.unpack('<%di' % a[3], struct.pack('<%dI' % a[3], *a[4:4 + a[3]])))))
    uc.hook_add(UC_HOOK_CODE, callin, begin=0x56C640, end=0x56C640)
    aims = []

    def slot_target(uc_, _a, _s, _d):
        a = struct.unpack('<3I', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP) + 4, 12))
        aims.append((now[0], a[2]))
    uc.hook_add(UC_HOOK_CODE, slot_target, begin=0x51A700, end=0x51A700)
    log = []
    for t in range(1, ticks + 1):
        now[0] = t
        put(uc, GS + 0x19F44, t)
        if t == order_tick:
            put(uc, unit + 0x60, MISSION)
        for address, args_, ecx in ((0x52AE90, (unit,), None), (0x56C870, (1,), vm),
                                    (0x4D8450, (unit,), None), (0x4DC800, (unit,), mover)):
            _, error = p.icd.call(address, args_, ecx=ecx)
            if error:
                raise RuntimeError((t, hex(address), error))
            if address == 0x52AE90 and t == switch_tick:
                _, error = p.icd.call(0x51A8B0, (unit, 1))
                if error or u32(uc, unit + 0x130) >> 30 != 1 or not u32(uc, unit + 0xD0) & 0x40:
                    raise RuntimeError((t, 'switch', error))
                if not keep_event:
                    put(uc, unit + 0xD0, u32(uc, unit + 0xD0) & ~0x40)
        if sweep and t % sweep == sweep_phase and (u32(uc, unit + 0x130) >> 18) & 3 == 2:
            index, marker = struct.unpack('<HH', uc.mem_read(record + 4, 4))
            if h16(uc, record + 0x1A) & 4 and not index and marker == 0x8000:
                inside, _ = p.icd.call(0x52ADF0, (unit, target, 0))
                reach, _ = p.icd.call(0x52AD60, (unit, target, 0))
                if reach and not inside:
                    p.icd.call(0x51A700, (unit, target, 0))
        x, _, z = struct.unpack('<3i', uc.mem_read(unit + 0x68, 12))
        log.append(dict(t=t, d=(((tx << 16) - x) ** 2 + ((tz << 16) - z) ** 2) ** 0.5 / 65536,
                        speed=s32(uc, mover + 0x20), stage=uc.mem_read(MISSION + 5, 1)[0],
                        slot=h16(uc, record + 4), heading=h16(uc, unit + 0x7E),
                        st={n: s32(uc, statics + 4 * n) for n in (2, 3, 5, 6, 7, 9, 10)}))
    if switch_tick:
        return log, calls, shots, aims
    return log, calls, shots


def switched(switch_tick, **options):
    """The tick slot 1 first got the target (51a700) after a switch at
    switch_tick."""
    _, _, _, aims = engagement(switch_tick=switch_tick, **options)
    return next((t for t, slot in aims if slot == 1 and t >= switch_tick), None)


def summarise(log, calls, shots):
    """The stop the first aim answers, and the walk controller's answer to it."""
    aim = next((t for t, name, _ in calls if name == 'AimWeapon'), None)
    fire = next((t for t, name, _ in calls if name == 'FireWeapon'), None)
    stop = None
    if aim:
        before = [r for r in log if r['t'] < aim]
        stop = next((before[i]['t'] for i in range(len(before) - 1, 0, -1)
                     if before[i]['speed'] == 0 and before[i - 1]['speed']), None)
    s = dict(stop=stop, aim=aim, fire=fire, shots=len(shots), d=log[-1]['d'])
    if stop:
        after = [r for r in log if r['t'] > stop]
        first = lambda test: next((r['t'] - stop for r in after if test(r)), None)  # noqa: E731
        s.update(stage1=first(lambda r: r['slot']), watcher=first(lambda r: not r['st'][3]),
                 aiming=first(lambda r: r['st'][2]), legs=first(lambda r: r['st'][9]),
                 restore=first(lambda r: r['st'][10]))
        s['turns'] = [(t - stop, v[0]) for t, name, v in calls if name == 'TurnDirection' and stop <= t <= stop + 6]
    return s


def _grid_case(job):
    order, distance, options = job
    return summarise(*engagement(order_tick=order, distance=distance, **options))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--distance', type=int, default=664)
    ap.add_argument('--offset', type=int, default=0)
    ap.add_argument('--order-tick', type=int, default=5)
    ap.add_argument('--ticks', type=int, default=600)
    ap.add_argument('--grid', action='store_true')
    ap.add_argument('--distances', type=int, nargs=2)
    ap.add_argument('--approach-shrink', type=int)
    ap.add_argument('--sweep', type=int, default=0)
    ap.add_argument('--sweep-phase', type=int, default=0)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--switch-tick', type=int, nargs='+')
    ap.add_argument('--no-event', action='store_true')
    args = ap.parse_args()
    if args.switch_tick:
        for t in args.switch_tick:
            got = switched(t, distance=args.distance, offset=args.offset, order_tick=args.order_tick,
                           ticks=max(args.ticks, t + 40), keep_event=not args.no_event)
            print(f'switch at {t}: 51a700 gives slot 1 the target at {got}'
                  f'{" (the same dispatch)" if got == t else ""}')
        return
    options = dict(ticks=args.ticks, sweep=args.sweep, sweep_phase=args.sweep_phase,
                   auto=bool(args.sweep), fire_state=2 if args.sweep else 0)
    if args.grid or args.distances:
        shrink = args.approach_shrink if args.approach_shrink is not None else (24 if args.grid else 0)
        options['approach_shrink'] = shrink
        jobs = ([(o, d, options) for o in range(5, 41) for d in (640, 656, 672, 688)] if args.grid else
                [(5, d, options) for d in range(args.distances[0], args.distances[1] + 1)])
        with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
            results = list(pool.map(_grid_case, jobs))
        fired = sum(r['fire'] is not None for r in results)
        print(f'{fired}/{len(results)} engagements fire (approach shrink {shrink}, sweep {args.sweep or "none"})')
        table = collections.Counter((r.get('watcher'), r.get('aiming'), r.get('legs'), r.get('restore'),
                                     r['fire'] is not None) for r in results)
        print('  watcher sees the stop / aiming / restore_legs / restore_x (ticks after the stop) -> fires')
        for (watcher, aiming, legs, restore, fires), n in sorted(table.items(), key=str):
            print(f'  +{watcher} / +{aiming} / {legs} / {restore} -> {fires}: {n}')
        return
    s = summarise(*engagement(distance=args.distance, offset=args.offset, order_tick=args.order_tick, **options))
    print(s)


if __name__ == '__main__':
    main()
