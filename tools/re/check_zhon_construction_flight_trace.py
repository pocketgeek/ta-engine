#!/usr/bin/env python3
"""Pair retail's placed-Zhon construction orbit with World flight movement.

The short check compares an installed native orbit point and mover. The optional
`--persistent` check runs the native 41ef00 dispatcher, real point-controller
factory/destructor, navigator binder, mover and commit over an unfinished placed
site, then pairs it with World construction through arrival and the next orbit
retarget. It is headless and does not launch the retail GUI.
"""
import argparse
import struct
import subprocess
from pathlib import Path

from emu import Icd, HEAP


def put(uc, address, *values):
    uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                      *(value & 0xffffffff for value in values)))


def signed_words(uc, address, count):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, count * 4))


def native_trace(steps, seed):
    p = Icd()
    uc = p.uc
    game, unit, site, kind, owner, mission = (HEAP + n * 0x10000
                                               for n in range(1, 7))
    mover = unit + 0x300
    navigator = HEAP + 0x80000
    settings, options, sectors = (HEAP + n * 0x10000 for n in range(9, 12))
    # Match the reported retail body height at this stage of the work cycle,
    # then let the actual mover converge toward the shared flat-plane cruise Y.
    start = (1000, 161, 1000)
    site_position = (1120, 100, 1060)
    terrain = 100

    put(uc, 0x62d55c, game)
    put(uc, 0x62d558, settings)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    put(uc, 0x62db84, game + 0x30000)
    put(uc, 0x65e108, 0)
    uc.mem_write(0x65e01c, b'\x01')
    uc.mem_write(0x65e02c, b'\x00')
    uc.mem_write(0x65e020, b'\x01')
    uc.mem_write(0x65e034, b'\x00')
    put(uc, 0x629680, 0)
    put(uc, 0x66e3d8, 1)
    put(uc, game + 0x19e90, 4096, 4096)
    put(uc, game + 0x19e98, 128, 128)
    put(uc, game + 0x19f30, 1000)
    put(uc, game + 0x19f44, 1000)
    put(uc, game + 0x175c4 + 0x126, kind)
    put(uc, 0x64186c, seed)

    # The native sector table stores one height byte per 128-pixel sector.
    sector_stride = 32
    put(uc, game + 0x19f18, sectors)
    put(uc, game + 0x19f1c, sector_stride)
    sector_data = bytearray(sector_stride * sector_stride * 10)
    sector_data[1::10] = bytes([terrain]) * (sector_stride * sector_stride)
    uc.mem_write(sectors, bytes(sector_data))
    sector = sectors + ((start[2] >> 7) * sector_stride + (start[0] >> 7)) * 10
    put(uc, unit + 0xa4, sector)
    put(uc, sector + 6, unit)

    put(uc, owner, 1)
    uc.mem_write(owner + 0xea, b'\x00')
    put(uc, unit + 0x08, mover)
    put(uc, unit + 0x60, mission)
    put(uc, unit + 0x68, *(value << 16 for value in start))
    uc.mem_write(unit + 0x74, struct.pack('<4h', start[0] // 16,
                 start[2] // 16, 2, 2))
    uc.mem_write(unit + 0x7e, struct.pack('<H', 0))
    put(uc, unit + 0xb4, kind)
    put(uc, unit + 0xb8, owner)
    put(uc, unit + 0x12b, 163840)     # 2.5 maxvelocity, exact, no speed spread
    put(uc, unit + 0x130, 0x01000000)

    # FBI values from zonhunt.fbi, stored in retail's native fixed-point fields.
    put(uc, kind + 0x162, 163840)
    put(uc, kind + 0x166, 13107)      # brakerate 0.2
    put(uc, kind + 0x16a, 32768)      # acceleration 0.5
    put(uc, kind + 0x16e, 65536)      # watermultiplier default 1.0
    put(uc, kind + 0x172, 0x13333)    # retail roadmultiplier default 1.2
    uc.mem_write(kind + 0x18e, struct.pack('<H', 400))
    put(uc, kind + 0x230, 100)        # builddistance
    uc.mem_write(kind + 0x23a, struct.pack('<h', 150))  # cruisealt
    put(uc, kind + 0x260, 0x800)      # canfly
    uc.mem_write(kind + 0x126, struct.pack('<HH', 2, 2))

    put(uc, mover, navigator)
    put(uc, mover + 0x20, 0)
    put(uc, mover + 0x30, 0x7fffffff)  # keep sector terrain scan fixed for trace
    uc.mem_write(mover + 0x36, struct.pack('<H', 2))
    put(uc, navigator, 0x5f34d4)
    put(uc, navigator + 8, unit)
    put(uc, navigator + 0x0c, *(value << 16 for value in start))
    uc.mem_write(navigator + 0x24, struct.pack('<H', 0))

    put(uc, site + 0x68, *(value << 16 for value in site_position))
    put(uc, site + 0xb4, kind)
    put(uc, site + 0x130, 0x01000000)
    put(uc, site + 0x108, 0x3f000000)  # incomplete construction site
    put(uc, mission + 0x0e, unit)
    put(uc, mission + 0x16, site)
    put(uc, mission + 4, 1)
    uc.mem_write(mission + 5, b'\x05')
    put(uc, mission + 6, 1)
    put(uc, mission + 10, 0xffffffff)
    put(uc, unit + 0x60, mission)

    # OS-only allocation/critical-section edges are the same substitutions as
    # probe_flying_construction_orbit.py. The handler, goal controller, setter,
    # navigator and mover all remain native retail code.
    stub_cs = HEAP + 0x3f0000
    stub_heap_alloc = HEAP + 0x3f0010
    heap_cursor = [HEAP + 0x3f1000]
    put(uc, 0x5eb118, stub_cs)
    put(uc, 0x5eb108, stub_cs)
    put(uc, 0x5eb110, stub_cs)
    put(uc, 0x5eb268, stub_heap_alloc)

    def one_arg_service(_uc, _sp):
        return 1, 0

    def heap_alloc(_uc, sp):
        requested = struct.unpack('<I', uc.mem_read(sp + 8, 4))[0]
        ptr = heap_cursor[0]
        size = (requested + 15) & ~15
        heap_cursor[0] += max(size, 16)
        uc.mem_write(ptr, bytes(size))
        return 3, ptr

    p.hooks.update({stub_cs: one_arg_service,
                    stub_heap_alloc: heap_alloc,
                    0x5d3d12: lambda _uc, _sp: (0, 0),
                    0x56c640: lambda _uc, _args: (8, 0)})
    p.freeze_hooks()

    result, error = p.call(0x41ef00, args=(unit, mission, 1))
    if error:
        raise RuntimeError(('41ef00 stage 5', error))
    if result != 4:
        raise AssertionError(f'41ef00 did not continue to orbit stage 4: {result}')
    result, error = p.call(0x41ef00, args=(unit, mission, 0))
    if error:
        raise RuntimeError(('41ef00 stage 4', error))
    active = struct.unpack('<I', uc.mem_read(navigator + 4, 4))[0]
    if not active:
        raise AssertionError('41ef00 did not install its controller on navigator')
    goal = signed_words(uc, active + 0x26, 3)
    flags, _, _, heading = struct.unpack('<4H', uc.mem_read(active + 8, 8))
    if flags != 0x60:
        raise AssertionError(f'expected construction controller flags 0x60, got {flags:#x}')
    if seed == 50 and (goal != (68223960, 6553600, 66009400) or heading != 43016):
        raise AssertionError({'seed-50 native orbit goal': goal, 'heading': heading})

    rows = []
    for tick in range(1, steps + 1):
        put(uc, game + 0x19f44, tick)
        _, error = p.call(0x4dc800, args=(unit,), ecx=mover)
        if error:
            raise RuntimeError({'tick': tick, '4dc800': error})
        position = signed_words(uc, unit + 0x68, 3)
        body_heading = struct.unpack('<H', uc.mem_read(unit + 0x7e, 2))[0]
        speed = struct.unpack('<i', uc.mem_read(mover + 0x20, 4))[0]
        velocity = signed_words(uc, mover + 8, 3)
        nav_out = signed_words(uc, navigator + 0x0c, 6)
        nav_heading = struct.unpack('<H', uc.mem_read(navigator + 0x24, 2))[0]
        rows.append((tick, *position, body_heading, speed, *velocity,
                     *nav_out, nav_heading,
                     position[0] - (site_position[0] << 16),
                     position[2] - (site_position[2] << 16)))
    metadata = {'goal': goal, 'goal_heading': heading, 'site': site_position,
                'start': start, 'terrain': terrain, 'max_speed': 163840}
    return metadata, rows


def world_trace(binary, install, steps, metadata):
    goal = metadata['goal']
    site = metadata['site']
    start = metadata['start']
    args = [binary, '--construction-flight-trace', install, str(steps),
            str(metadata['terrain']), *(str(value << 16) for value in start),
            '0', str(goal[0]), str(goal[2]), str(metadata['goal_heading']),
            str(site[0] << 16), str(site[2] << 16), str(metadata['max_speed'])]
    result = subprocess.run(args, check=True, capture_output=True, text=True)
    lines = result.stdout.splitlines()
    profile = tuple(map(int, lines[0].split()[1:]))
    rows = [tuple(map(int, line.split())) for line in lines[1:]]
    if len(rows) != steps:
        raise AssertionError(f'World emitted {len(rows)} rows, expected {steps}')
    return profile, rows


def native_persistent_trace(steps, seed):
    """Run the live retail mission dispatcher until its unfinished site waits."""
    p = Icd()
    uc = p.uc
    game, unit, site, kind, owner, mission = (HEAP + n * 0x10000
                                               for n in range(1, 7))
    mover = unit + 0x300
    navigator = HEAP + 0x80000
    settings, options, sectors = (HEAP + n * 0x10000 for n in range(9, 12))
    defs = HEAP + 0x70000
    start = (1024, 161, 1000)
    site_position = (1120, 100, 1060)
    terrain = 100

    put(uc, 0x62d55c, game)
    put(uc, 0x62d558, settings)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    put(uc, 0x62db84, defs)
    put(uc, defs + 25 + 4, 0x41ef00)
    put(uc, 0x65e108, 0)
    for address, value in ((0x65e01c, 1), (0x65e02c, 0),
                           (0x65e020, 1), (0x65e034, 0)):
        uc.mem_write(address, bytes((value,)))
    put(uc, 0x629680, 0)
    put(uc, 0x66e3d8, 1)
    put(uc, game + 0x19e90, 4096, 4096)
    put(uc, game + 0x19e98, 128, 128)
    put(uc, game + 0x19f30, 1000)
    put(uc, game + 0x19f44, 0)
    put(uc, game + 0x175c4 + 0x126, kind)
    put(uc, 0x64186c, (seed ^ 0x66e29572) | 1)

    sector_stride = 32
    put(uc, game + 0x19f18, sectors)
    put(uc, game + 0x19f1c, sector_stride)
    sector_data = bytearray(sector_stride * sector_stride * 10)
    sector_data[1::10] = bytes([terrain]) * (sector_stride * sector_stride)
    uc.mem_write(sectors, bytes(sector_data))
    sector = sectors + ((start[2] >> 7) * sector_stride + (start[0] >> 7)) * 10
    put(uc, unit + 0xa4, sector)
    put(uc, sector + 6, unit)

    put(uc, owner, 1)
    uc.mem_write(owner + 0xea, b'\x00')
    put(uc, unit + 0x08, mover)
    put(uc, unit + 0x60, mission)
    put(uc, unit + 0x68, *(value << 16 for value in start))
    uc.mem_write(unit + 0x74, struct.pack('<4h', start[0] // 16,
                 start[2] // 16, 2, 2))
    uc.mem_write(unit + 0x7e, struct.pack('<H', 0))
    put(uc, unit + 0xb4, kind)
    put(uc, unit + 0xb8, owner)
    put(uc, unit + 0x12b, 163840)
    put(uc, unit + 0x130, 0x01000000)
    for offset, value in ((0x162, 163840), (0x166, 13107),
                          (0x16a, 32768), (0x16e, 65536),
                          (0x172, 0x13333), (0x230, 100),
                          (0x260, 0x800)):
        put(uc, kind + offset, value)
    uc.mem_write(kind + 0x18e, struct.pack('<H', 400))
    uc.mem_write(kind + 0x23a, struct.pack('<h', 150))
    uc.mem_write(kind + 0x126, struct.pack('<HH', 2, 2))

    put(uc, mover, navigator)
    put(uc, mover + 0x20, 0)
    put(uc, mover + 0x30, 0x7fffffff)
    uc.mem_write(mover + 0x36, struct.pack('<H', 2))
    put(uc, navigator, 0x5f34d4)
    put(uc, navigator + 8, unit)
    put(uc, navigator + 0x0c, *(value << 16 for value in start))
    uc.mem_write(navigator + 0x24, struct.pack('<H', 0))

    put(uc, site + 0x68, *(value << 16 for value in site_position))
    put(uc, site + 0xb4, kind)
    put(uc, site + 0x130, 0x01000000)
    put(uc, site + 0x108, 0x3f000000)
    put(uc, mission + 0x0e, unit)
    put(uc, mission + 0x16, site)
    put(uc, mission + 4, 1)
    uc.mem_write(mission + 5, b'\x04')  # dispatcher creates the first orbit point
    put(uc, mission + 6, 0)
    put(uc, mission + 10, 0xffffffff)
    put(uc, unit + 0x60, mission)

    # OS allocation and synchronization boundaries only. In particular there
    # are no hooks on the point-controller factory or mission/navigator binder.
    stub_cs = HEAP + 0x3f0000
    stub_heap_alloc = HEAP + 0x3f0010
    stub_heap_free_a = HEAP + 0x3f0020
    stub_heap_free_b = HEAP + 0x3f0030
    heap_cursor = [HEAP + 0x3f1000]
    put(uc, 0x5eb118, stub_cs)
    put(uc, 0x5eb108, stub_cs)
    put(uc, 0x5eb110, stub_cs)
    put(uc, 0x5eb268, stub_heap_alloc)
    put(uc, 0x5eb26c, stub_heap_free_a)
    put(uc, 0x5eb270, stub_heap_free_b)

    def one_arg_service(_uc, _sp):
        return 1, 0

    def heap_alloc(_uc, sp):
        requested = struct.unpack('<I', uc.mem_read(sp + 8, 4))[0]
        ptr = heap_cursor[0]
        size = (requested + 15) & ~15
        heap_cursor[0] += max(size, 16)
        uc.mem_write(ptr, bytes(size))
        return 3, ptr

    def heap_free(_uc, _sp):
        # The native fixture uses a bump allocator, so release is an inert OS edge.
        return 3, 1

    native_callback_events = []
    native_callback_rows = []

    def capture_callback(uc, sp):
        args = struct.unpack('<8I', uc.mem_read(sp, 32))
        name = bytes(uc.mem_read(args[0], 64)).split(b'\0')[0].decode('ascii')
        values = tuple(struct.unpack('<i', struct.pack('<I', value))[0]
                       for value in args[4:4 + args[3]])
        native_callback_events.append((name, values))
        return 8, 0

    p.hooks.update({stub_cs: one_arg_service,
                    stub_heap_alloc: heap_alloc,
                    stub_heap_free_a: heap_free,
                    stub_heap_free_b: heap_free,
                    # Construction accounting is deliberately unfinished so
                    # the placed site and its active mission persist.
                    0x429af0: lambda _uc, _sp: (3, 0),
                    0x5d3d12: lambda _uc, _sp: (0, 0),
                    0x56c640: capture_callback})
    if 0x4e40e0 in p.hooks or 0x4d4d40 in p.hooks:
        raise AssertionError('persistent fixture must use native factory and binder')
    p.freeze_hooks()

    rows = []
    arrival_phases = None
    for tick in range(1, steps + 1):
        put(uc, game + 0x19f44, tick)
        _, error = p.call(0x4d8450, args=(unit,))
        if error:
            raise RuntimeError({'tick': tick, '4d8450 dispatcher': error})
        stage = uc.mem_read(mission + 5, 1)[0]
        wait_mask = get_dword(uc, mission + 6)
        deadline = get_dword(uc, mission + 10)
        if tick == 82:
            arrival_phases = {'after_dispatch':
                              (get_dword(uc, navigator + 4),
                               get_dword(uc, mission + 0x6a),
                               signed_words(uc, unit + 0x68, 3))}
        native_callback_events.clear()
        _, error = p.call(0x4dc800, args=(unit,), ecx=mover)
        if error:
            raise RuntimeError({'tick': tick, '4dc800': error})
        native_callback_rows.append((tick, tuple(native_callback_events)))
        if tick == 82:
            arrival_phases['after_mover'] = (get_dword(uc, navigator + 4),
                                             get_dword(uc, mission + 0x6a),
                                             signed_words(uc, unit + 0x68, 3))
        _, error = p.call(0x51b2a0, args=(unit,), ecx=mover)
        if error:
            raise RuntimeError({'tick': tick, '51b2a0': error})
        if tick == 82:
            arrival_phases['after_commit'] = (get_dword(uc, navigator + 4),
                                              get_dword(uc, mission + 0x6a),
                                              signed_words(uc, unit + 0x68, 3))
        active = get_dword(uc, navigator + 4)
        goal = (signed_words(uc, active + 0x26, 3) if active else (0, 0, 0))
        flags, _, _, heading = (struct.unpack('<4H', uc.mem_read(active + 8, 8))
                                if active else (0, 0, 0, 0))
        position = signed_words(uc, unit + 0x68, 3)
        body_heading = struct.unpack('<H', uc.mem_read(unit + 0x7e, 2))[0]
        speed = struct.unpack('<i', uc.mem_read(mover + 0x20, 4))[0]
        velocity = signed_words(uc, mover + 8, 3)
        nav_out = signed_words(uc, navigator + 0x0c, 6)
        nav_heading = struct.unpack('<H', uc.mem_read(navigator + 0x24, 2))[0]
        pending = get_dword(uc, mission + 0x6a)
        rows.append((tick, *position, body_heading, speed, *velocity,
                     *nav_out, nav_heading,
                     position[0] - (site_position[0] << 16),
                     position[2] - (site_position[2] << 16),
                     *goal, flags, heading, int(bool(active)), stage,
                     wait_mask, deadline, pending, get_dword(uc, 0x64186c)))
    metadata = {'start': start, 'site': site_position, 'terrain': terrain,
                'speed': 163840, 'heading': 0}
    return metadata, rows, arrival_phases, native_callback_rows


def get_dword(uc, address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]


def world_persistent_trace(binary, install, steps, seed, metadata):
    start = metadata['start']
    site = metadata['site']
    args = [binary, '--persistent-construction-flight-trace', install,
            str(steps), str(metadata['terrain']),
            *(str(value << 16) for value in start), str(metadata['heading']),
            str(site[0] << 16), str(site[2] << 16), str(metadata['speed']), str(seed)]
    result = subprocess.run(args, check=True, capture_output=True, text=True)
    lines = result.stdout.splitlines()
    profile = tuple(map(int, lines[0].split()[1:]))
    rows, events = [], []
    for line in lines[1:]:
        fields = line.split()
        if fields[0] == 'TRACE':
            rows.append(tuple(map(int, fields[1:])))
        elif fields[0] == 'RNG':
            events.append(tuple(map(int, fields[1:])))
    if len(rows) != steps:
        raise AssertionError(f'World emitted {len(rows)} rows, expected {steps}')
    return profile, rows, events


def retail_random(seed, bound):
    if bound < 2:
        return seed, 0
    next_seed = (seed % 127773) * 16807 - (seed // 127773) * 2836
    if next_seed <= 0:
        next_seed += 0x7fffffff
    return next_seed, next_seed % bound


def cob_methods(unit):
    script_dir = Path(__file__).resolve().parents[2] / 'assets/extracted/all/scripts'
    path = script_dir / f'{unit}.cob'
    data = path.read_bytes()
    header = struct.unpack_from('<10I', data)
    names = []
    for index in range(header[1]):
        offset = struct.unpack_from('<I', data, header[7] + 4 * index)[0]
        names.append(data[offset:].split(b'\0', 1)[0].decode('ascii'))
    return set(names)


def expected_persistent_rng(seed, steps):
    state = (seed ^ 0x66e29572) | 1
    events, states = [], []
    for tick in range(1, steps + 1):
        if tick == 1:
            bounds = (9362, 9362, 8, 8)
        else:
            state, value = retail_random(state, 50)
            events.append((tick, 50, value))
            bounds = (9362, 9362, 8, 8) if value == 0 else ()
        for bound in bounds:
            before = state
            state, value = retail_random(state, bound)
            events.append((tick, bound, value))
        states.append(state)
    return events, states


def run_persistent_check(binary, install):
    # Seed 1 stays outside the first arrival window until after two successful
    # retargets, then exposes native controller release at tick 82 and the next
    # mission target selection at tick 101.
    seed, steps = 1, 103
    metadata, retail, arrival_phases, native_callbacks = native_persistent_trace(steps, seed)
    profile, world, world_rng = world_persistent_trace(binary, install, steps,
                                                        seed, metadata)
    expected_profile = (163840, 32768, 13107, 0x13333, 400, 150, 100)
    if profile != expected_profile:
        raise AssertionError({'World zonhunt profile': profile,
                              'expected asset profile': expected_profile})
    movement_callbacks = {'TurnDirection', 'MoveRate', 'setSFXoccupy'}
    declared_callbacks = cob_methods('zonhunt').intersection(movement_callbacks)
    if declared_callbacks != {'setSFXoccupy'}:
        raise AssertionError({'zonhunt movement callback declarations':
                              sorted(declared_callbacks)})
    expected_initial_requests = [('TurnDirection', (-91,)),
                                 ('MoveRate', (3,)),
                                 ('setSFXoccupy', (5,))]
    if (len(native_callbacks) != steps or native_callbacks[0] !=
            (1, tuple(expected_initial_requests))):
        raise AssertionError({'native zonhunt first-mover callback phase':
                              native_callbacks[:1],
                              'expected': [(1, tuple(expected_initial_requests))]})
    native_occupancy_edges = [(tick, values[0])
                              for tick, events in native_callbacks
                              for name, values in events
                              if name == 'setSFXoccupy' and name in declared_callbacks]
    world_occupancy_edges = [(row[0], row[30]) for row in world if row[31]]
    if native_occupancy_edges != world_occupancy_edges:
        raise AssertionError({'Zhon monarch occupancy callback edges':
                              {'retail 0x4dc800': native_occupancy_edges,
                               'World render helper': world_occupancy_edges}})
    expected_events, expected_states = expected_persistent_rng(seed, steps)
    actual_events = [(tick, bound, result)
                     for tick, bound, _before, _after, result in world_rng]
    if actual_events != expected_events:
        raise AssertionError({'World construction RNG events': actual_events,
                              'expected': expected_events})
    for tick, (native_row, world_row, expected_state) in enumerate(
            zip(retail, world, expected_states), 1):
        # Body and navigator fields remain paired while native has released
        # its point controller. World retains the mission's last orbit point
        # locally until the next one is selected.
        if native_row[:18] != world_row[:18]:
            raise AssertionError({'tick': tick,
                                  'retail movement/navigation': native_row[:18],
                                  'World movement/navigation': world_row[:18]})
        if native_row[23] and native_row[18:23] != world_row[18:23]:
            raise AssertionError({'tick': tick,
                                  'retail active orbit goal': native_row[18:23],
                                  'World orbit goal': world_row[18:23]})
        if native_row[-1] != expected_state or world_row[26] != expected_state:
            raise AssertionError({'tick': tick, 'native RNG': native_row[-1],
                                  'World RNG': world_row[26],
                                  'expected RNG': expected_state})
        expected_pending = 0x500 if 82 <= tick < 101 else 0
        expected_active = int(tick < 82 or tick >= 101)
        if (native_row[23] != expected_active or
                native_row[24:27] != (5, 0xb, tick + 1) or
                native_row[27] != expected_pending):
            raise AssertionError({'tick': tick, 'native mission state': native_row[24:28],
                                  'expected controller/stage/wait/deadline/pending':
                                  (expected_active, 5, 0xb, tick + 1,
                                   expected_pending)})
        if (world_row[23:26] != (1, 2, 2) or world_row[27] == 0 or
                world_row[28:30] != (1, 1)):
            raise AssertionError({'tick': tick, 'World persistent site/job':
                                  world_row[23:26], 'RNG event count': world_row[27],
                                  'build order count/presence': world_row[28:30]})
    native_goal_ticks = [tick for tick in range(1, steps + 1)
                         if retail[tick - 1][23] and
                         (tick == 1 or not retail[tick - 2][23] or
                          retail[tick - 1][18:23] != retail[tick - 2][18:23])]
    world_goal_ticks = [tick for tick in range(1, steps + 1)
                        if tick == 1 or
                        world[tick - 1][18:23] != world[tick - 2][18:23]]
    if native_goal_ticks != [1, 25, 63, 101] or world_goal_ticks != [1, 25, 63, 101]:
        raise AssertionError({'native target selection ticks': native_goal_ticks,
                              'World target selection ticks': world_goal_ticks,
                              'expected': [1, 25, 63, 101]})

    native82, world82 = retail[81], world[81]
    native81, world81 = retail[80], world[80]
    after_dispatch = arrival_phases['after_dispatch']
    after_mover = arrival_phases['after_mover']
    after_commit = arrival_phases['after_commit']
    rng82_after, rng82_value = retail_random(native81[-1], 50)
    rng82_events = [event for event in world_rng if event[0] == 82]
    expected_rng82_event = (82, 50, native81[-1], rng82_after, rng82_value)
    # The old default (nonpersistent) movement path returned at the accepted
    # temporary buildType hover goal before calling tickFlightBody. This checks
    # that World applies the native arrival-tick body step after controller
    # release while leaving the placed-build mission intact.
    if (native82[23] != 0 or native82[24:27] != (5, 0xb, 83) or
            native82[27] != 0x500 or world82[23:26] != (1, 2, 2) or
            native82[-1] != world82[26] or after_dispatch[0] == 0 or
            after_dispatch[1] != 0 or after_mover[0] != 0 or
            after_mover[1] != 0x500 or after_commit != after_mover or
            native82[-1] != rng82_after or rng82_events != [expected_rng82_event] or
            after_dispatch[2] != native81[1:4] or
            after_mover[2] != native82[1:4] or
            native82[:18] != world82[:18] or
            native82[1:4] == native81[1:4] or world82[1:4] == world81[1:4] or
            world82[18:23] != world81[18:23] or world82[28:30] != (1, 1)):
        raise AssertionError({'tick-82 arrival boundary':
                              {'native active/stage/wait/deadline/pending': native82[23:28],
                               'native phases': arrival_phases,
                               'World site/build/hover': world82[23:26],
                               'native RNG': native82[-1], 'World RNG': world82[26],
                               'World RNG events': rng82_events,
                               'expected tick-82 RNG event': expected_rng82_event}})
    native101, world101 = retail[100], world[100]
    if (native101[23] != 1 or native101[18:23] !=
            (66524080, 16384000, 69721840, 0x60, 49572) or
            world101[18:23] != native101[18:23] or
            native101[:18] != world101[:18]):
        raise AssertionError({'tick-101 next hover target':
                              {'native': native101[:24], 'World': world101[:24]}})
    print('PASS: 103 exact native/World persistent Zhon construction ticks, '
          'including orbit retargets at 25 and 63, arrival at 82, and the next '
          'target at 101; RNG and movement/navigation match each tick.')
    print(f'PASS: zonhunt declares {sorted(declared_callbacks)}; native 0x4dc800 '
          f'and World occupancy helper agree on callback edges {world_occupancy_edges}.')
    print('ARRIVAL: native tick 82 detaches its active point controller '
          f'{after_dispatch[0]:#x}->0 and posts pending 0x{after_dispatch[1]:x}'
          f'->0x{after_mover[1]:x}; the mover still applies body step '
          f'{native81[1:4]}->{native82[1:4]} (commit preserves 0x{after_commit[1]:x}). '
          'World now applies that identical step despite the temporary buildType '
          'hover order, keeps its site/job active while braking, and selects the '
          'same next hover goal at tick 101. Ground-builder movement remains on '
          'its separate path.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/conjure_test')
    parser.add_argument('--install', default='assets/game')
    parser.add_argument('--steps', type=int, default=16)
    parser.add_argument('--seed', type=lambda s: int(s, 0), default=50)
    parser.add_argument('--persistent', action='store_true',
                        help='run the native-dispatcher / persistent-build trace through arrival and retarget')
    args = parser.parse_args()
    if args.persistent:
        run_persistent_check(args.binary, args.install)
        return
    steps = max(1, min(args.steps, 10000))
    metadata, retail = native_trace(steps, args.seed)
    profile, world = world_trace(args.binary, args.install, steps, metadata)
    expected_profile = (163840, 32768, 13107, 0x13333, 400, 150, 100)
    if profile != expected_profile:
        raise AssertionError({'World zonhunt profile': profile,
                              'expected asset profile': expected_profile})
    for tick, (native_row, world_row) in enumerate(zip(retail, world), 1):
        if native_row != world_row:
            raise AssertionError({'tick': tick, 'retail': native_row,
                                  'World': world_row})
    if steps >= 16 and (retail[0][2] != 162 * 65536 or
                        retail[15][2] != 177 * 65536):
        raise AssertionError('expected the 161-to-177 world-unit altitude climb')
    print(f"PASS: {steps} exact native/World Zhon placed-construction flight ticks; "
          f"native 41ef00 goal {metadata['goal']} (heading {metadata['goal_heading']}) "
          "is bound through the native point-controller factory and navigator to 4dc800/524af0; "
          "XYZ, Y altitude, heading, velocity, navigator output, and site-relative X/Z match")


if __name__ == '__main__':
    main()
