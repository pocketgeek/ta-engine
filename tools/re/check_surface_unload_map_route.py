#!/usr/bin/env python3
"""Compare a boat unload route on a shipped map with retail's native search.

The C++ fixture loads the real TNT terrain and feature plane into World, then
exports the effective grades from the actual unload-circle request. Unicorn runs
retail's original circle constructor, reachability tracer, cost search and route
reconstruction against either those grades or native 0x508cd0 grades generated
from the TNT cell plane. An optional mover trace steps both implementations over
the shipped map without launching a game GUI.
An optional live-unload composition keeps the native mission and cargo active
through map-backed movement, arrival wakeup, passenger placement and release.
An optional blocker trace mirrors one mobile shore occupant through the actual
native placement routine and the unload retry. A dynamic-route case blocks a
carrier mid-trip and verifies its replacement route; an optional native worker
replay sends that captured request through retail's asynchronous singleton.
"""
import argparse
import os
import re
import struct
import subprocess
from pathlib import Path

from balance_inputs import class_record, properties, unit_properties
from emuphase import Phase, OBJ, TYPE, GS
from check_surface_unload_map_grades import (
    asset as retail_asset, native_grade_reader, native_water_profile)
from check_surface_unload_map_release import (
    cat, movement_profile, native_placement_oracle, parse_tnt)


def expected_path_costs(turn_rate, footprint, road_mult, water_mult,
                        movement_flags, heavy_slope):
    """Reconstruct the World retail-cost inputs from its unit profile."""
    ground, road, slope, traffic = 24, 8, (320 if heavy_slope else 48), 80
    if road_mult >= 81920:
        ground = 24 * road_mult // 65536
        slope = slope * road_mult // 65536
        traffic = 80 * road_mult // 65536

    effective_turn = turn_rate
    if movement_flags & 0x800:
        effective_turn = (turn_rate * road_mult // 65536) & 0xFFFF
    elif movement_flags & 0x1000:
        effective_turn = (turn_rate * water_mult // 65536) & 0xFFFF
    min_straight = footprint + 3
    if effective_turn >= 1000:
        short_turn = 16
        min_straight = footprint
    elif effective_turn > 200:
        short_turn = 16 + 120 * (1000 - effective_turn) // 800
        min_straight = footprint + 3 * (1000 - effective_turn) // 800
    else:
        short_turn = 136
    if heavy_slope:
        short_turn *= 5
        min_straight *= 2
    return ground, road, slope, traffic, short_turn, min_straight


def retail_unit_profile(hpitool, retail_root, unit, crusades=False):
    """Resolve the balance-selected shipped FBI, falling back to base FBI."""
    unit = unit.lower()
    if crusades:
        try:
            return unit_properties(retail_asset(
                hpitool, retail_root, f'unitscb/{unit}.fbi').decode('latin1'))
        except subprocess.CalledProcessError:
            pass
    return unit_properties(retail_asset(
        hpitool, retail_root, f'units/{unit}.fbi').decode('latin1'))


def native_detached_passenger_occupancy_probe(p, carrier, owner,
                                               passenger_profile,
                                               passenger_fields,
                                               moveinfo_text,
                                               released_position,
                                               second_passenger_id,
                                               placement_args):
    """Run retail placement against one detached Araarch in this same map Phase."""
    foot_x, foot_z = passenger_profile[:2]
    class_name = passenger_fields.get('movementclass', '').lower()
    class_fields = next((properties(block_text) for block_text in
        re.findall(r'\[[^]]+\]\s*\{([^{}]*)\}', moveinfo_text, re.S)
        if properties(block_text).get('name', '').lower() == class_name), None)
    if class_fields is None:
        raise AssertionError(('missing detached passenger movement class', class_name))
    class_bytes = class_record(class_fields)

    # The live carrier already occupies id 1. Retail addresses an entity by
    # base + id * 0x138, so the detached passenger is installed at id 2.
    entity_base = carrier - 0x138
    passenger_id = 2
    passenger = entity_base + passenger_id * 0x138
    entity_end = entity_base + 4 * 0x138
    first_page = entity_base & ~0xfff
    if not any(begin <= first_page and end >= first_page + 0xfff
               for begin, end, _perms in p.uc.mem_regions()):
        p.uc.mem_map(first_page, 0x1000)
    p.uc.mem_write(entity_base, bytes(0x138))
    p.uc.mem_write(passenger, bytes(0x138))
    p.uc.mem_write(GS + 0x14e84, struct.pack('<II', entity_base, entity_end))
    p.uc.mem_write(owner + 0x74, struct.pack('<I', entity_base))
    p.uc.mem_write(owner + 0x78, struct.pack('<I', entity_end))

    mover, nav, kind, grid = (p._alloc(n) for n in (0x180, 0x180, 0x400, 0x40))
    for address, size in ((mover, 0x180), (nav, 0x180),
                          (kind, 0x400), (grid, 0x40)):
        p.uc.mem_write(address, bytes(size))
    put = lambda address, fmt, *values: p.uc.mem_write(
        address, struct.pack(fmt, *values))
    p.uc.mem_write(grid + 4, class_bytes)
    put(kind + 0x18a, '<I', grid)
    put(kind + 0x126, '<hh', foot_x, foot_z)
    put(kind + 0x192, '<hh', int(class_fields.get('maxwaterdepth', '10000')),
        int(class_fields.get('minwaterdepth', '-10000')))
    p.uc.mem_write(kind + 0x23c,
                   bytes((class_bytes[12], class_bytes[14])))
    p.uc.mem_write(kind + 0x24a, b'\x01')
    for key, offset, default in (('maxvelocity', 0x162, 0),
                                 ('brakerate', 0x166, 0.5),
                                 ('acceleration', 0x16a, 0.5)):
        put(kind + offset, '<i', int(float(passenger_fields.get(key, default)) * 65536))
    for key, offset, default in (('turnrate', 0x18e, 500),
                                 ('turninplacerate', 0x190, 0)):
        put(kind + offset, '<H', int(float(passenger_fields.get(key, default))) & 65535)
    put(kind + 0x172, '<i', int(float(passenger_fields.get('roadmultiplier', 1.2)) * 65536))
    put(kind + 0x16e, '<i', int(float(passenger_fields.get(
        'watermultiplier', 1)) * 65536))
    put(kind + 0x249, '<B', 6)

    put(passenger + 2, '<H', passenger_id)
    put(passenger + 8, '<I', mover)
    put(passenger + 0x68, '<iii', *released_position)
    origin_x = (released_position[0] // 65536 - (foot_x - 1) * 8) // 16
    origin_z = (released_position[2] // 65536 - (foot_z - 1) * 8) // 16
    put(passenger + 0x74, '<hh', origin_x, origin_z)
    put(passenger + 0x78, '<hh', foot_x, foot_z)
    put(passenger + 0xa4, '<I', 0)
    put(passenger + 0xa8, '<I', 0)
    put(passenger + 0xb4, '<I', kind)
    put(passenger + 0xb8, '<I', owner)
    # Retail's low flag bits are the mover/ground state. A live detached ground
    # Araarch is in state 1; mode 0 is not registered by the native cell updater.
    put(passenger + 0x130, '<I', 0x1000001)
    put(mover, '<I', nav)
    put(nav, '<I', 0x5f2a24)
    put(nav + 8, '<I', passenger)
    put(mover + 0x20, '<i', 0)
    put(mover + 0x30, '<I', 0x7fffffff)
    put(mover + 0x36, '<H', 0)

    occupied_cells = []
    for z in range(origin_z, origin_z + foot_z):
        for x in range(origin_x, origin_x + foot_x):
            occupied_cells.append((x, z))

    # Register this live entity through the same native cell/sector updater
    # invoked by retail's mover, rather than planting its four cell ids from
    # Python. This also seeds the entity's native sector-list links.
    _, registration_error = p.icd.call(0x5066f0, (passenger,))
    assert registration_error is None, ('native detached Araarch cell registration',
                                         registration_error)
    registered_cell_ids = [struct.unpack('<H', p.uc.mem_read(
        p.cells_addr + (z * p.W + x) * 14, 2))[0] for x, z in occupied_cells]
    assert registered_cell_ids == [passenger_id] * len(occupied_cells), (
        'native detached Araarch cell registration did not populate its footprint',
        occupied_cells, registered_cell_ids)

    args = list(placement_args)
    args[1] = second_passenger_id

    def native_place(call_args):
        # GROUND_UNLOAD's first stack value is not the passenger type block.
        return p.icd.call(0x507d10,
                          (kind, call_args[1], call_args[2],
                           call_args[3], call_args[4]))

    hooks = p.icd.hooks
    placement_hook = hooks.pop(0x507d10, None)
    try:
        exact_result, exact_error = native_place(args)
        relaxed_args = list(args)
        relaxed_args[4] = 1
        relaxed_result, relaxed_error = native_place(relaxed_args)
        packed = args[2] & 0xffffffff
        cell_x, cell_z = struct.unpack('<hh', struct.pack('<I', packed))
        nearby = {}
        for dx, dz in ((-2, 0), (2, 0), (0, -2), (0, 2)):
            candidate = list(args)
            candidate[2] = ((cell_x + dx) & 0xffff) | (((cell_z + dz) & 0xffff) << 16)
            nearby[(dx, dz)] = native_place(candidate)
    finally:
        if placement_hook is not None:
            hooks[0x507d10] = placement_hook
    if exact_error:
        raise RuntimeError(('same-Phase native 0x507d10 exact occupied-site query',
                            exact_error))
    if relaxed_error:
        raise RuntimeError(('same-Phase native 0x507d10 relaxed occupied-site query',
                            relaxed_error))
    if any(error for _result, error in nearby.values()):
        raise RuntimeError(('same-Phase native 0x507d10 nearby-site query', nearby))
    print(f'  Same-Phase occupancy probe: entity id {passenger_id} at slot '
          f'{hex(passenger)} in [{hex(entity_base)}, {hex(entity_end)}), '
          f'occupying {occupied_cells}; native exact second-passenger candidate '
          f'returned {exact_result} (allow-moving query {relaxed_result}); '
          f'neighboring two-cell candidates returned '
          f'{ {offset: result for offset, (result, _error) in nearby.items()} }.')

    print(f'  Native cell registration 0x5066f0: entity mode=1, sector link='
          f'{hex(struct.unpack("<I", p.uc.mem_read(passenger + 0xa4, 4))[0])}; '
          f'footprint cell ids={registered_cell_ids}.')
    return exact_result, nearby, [], (passenger_id, registered_cell_ids), kind


def replay_native_attempt(width, height, cached_grades, live_grade, attempt,
                          profile, target_x, target_z, circle_radius, sea,
                          position):
    """Run one captured World request through retail's actual search kernel."""
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

    (tick, unit_id, sx, sz, goal_x, goal_z, heading, retry, weight, *_rest) = attempt
    (turn, fx, fz, road, water, flags, _transport_dist, max_water, min_water,
     half_cell_ticks, heavy) = profile
    goal_cell = (goal_x - fx // 2, goal_z - fz // 2)
    phase = Phase(width, height)
    unit = phase.unit(sx - fx // 2, sz - fz // 2)
    assert phase.construct() is None
    phase.plant_request(unit, (sx - fx // 2, sz - fz // 2), goal_cell)
    phase.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
    phase.uc.mem_write(unit + 0x68, struct.pack('<iii', position[0],
                                                sea * 65536, position[1]))
    phase.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
    mover = struct.unpack('<I', phase.uc.mem_read(unit + 8, 4))[0]
    phase.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
    phase.uc.mem_write(TYPE + 0x126, struct.pack('<hh', fx, fz))
    phase.uc.mem_write(TYPE + 0x18e, struct.pack('<H', turn))
    phase.uc.mem_write(TYPE + 0x172, struct.pack('<i', road))
    phase.uc.mem_write(TYPE + 0x260, struct.pack('<I', 0x80000 if heavy else 0))
    phase.uc.mem_write(TYPE + 0x16e, struct.pack('<i', water))
    phase.uc.mem_write(TYPE + 0x192, struct.pack('<hh', max_water, min_water))
    phase.uc.mem_write(TYPE + 0x249, bytes([half_cell_ticks]))
    phase.uc.mem_write(OBJ + 0x1ad, struct.pack('<I', retry))
    phase.uc.mem_write(phase.GRID + 4, struct.pack('<hh', fx, fz))
    phase.set_grade_plane(cached_grades)
    _, error = phase.icd.call(0x4e2500,
        (phase.HANDLE + 0x1000, target_x * 65536, target_z * 65536,
         circle_radius), ecx=phase.HANDLE)
    assert error is None, error
    native_goal = tuple(struct.unpack('<hh', phase.uc.mem_read(phase.HANDLE + 8, 4)))
    native_radius, radius_squared = struct.unpack('<ii',
        phase.uc.mem_read(phase.HANDLE + 0x0c, 8))
    expected_radius_squared = int(circle_radius * circle_radius / 256 + 0.5)
    assert (native_goal, native_radius, radius_squared) == (
        goal_cell, circle_radius, expected_radius_squared), (
            native_goal, native_radius, radius_squared,
            goal_cell, circle_radius, expected_radius_squared)

    dispatch_calls = [0]
    live_grade_calls = [0]
    grade_results = []
    pending_grade_queries = []

    def observe_dispatch(_uc, _address, _size, _data):
        dispatch_calls[0] += 1
        esp = _uc.reg_read(UC_X86_REG_ESP)
        _return, x, z, _direction = struct.unpack(
            '<Iiii', _uc.mem_read(esp, 16))
        pending_grade_queries.append((x, z))

    def observe_grade_return(uc, address, _size, _data):
        assert pending_grade_queries, hex(address)
        x, z = pending_grade_queries.pop()
        grade_results.append((x, z, uc.reg_read(UC_X86_REG_EAX), address))

    def query_live_grade(_uc, args):
        live_grade_calls[0] += 1
        _who, world_x, _world_y, world_z = struct.unpack(
            '<Iiii', phase.uc.mem_read(args, 16))
        x = ((world_x >> 19) - fx) // 2
        z = ((world_z >> 19) - fz) // 2
        if not (0 <= x < width and 0 <= z < height):
            return 4, 0
        return 4, live_grade(x, z)

    def no_gate(_uc, _args):
        return 0, 0

    def visible(_uc, _args):
        return 4, 1

    def no_special_body(_uc, _args):
        return 2, 0

    def request_weight(_uc, args):
        address = struct.unpack('<I', phase.uc.mem_read(args, 4))[0]
        phase.uc.mem_write(address, struct.pack('<I', weight))
        return 1, address

    # Preserve retail's cached-grade and grade-2 live-refresh dispatch. Only
    # the dynamic terrain/body query comes from the independent map oracle.
    phase.icd.hooks[0x413c80] = visible
    phase.icd.hooks[0x409fe0] = no_gate
    phase.icd.hooks[0x4db640] = query_live_grade
    phase.icd.hooks[0x50e600] = no_special_body
    phase.uc.hook_add(UC_HOOK_CODE, observe_dispatch,
                      begin=0x4139d0, end=0x4139d0)
    for return_site in (0x4139fe, 0x413a51, 0x413a9a, 0x413ae1,
                        0x413bb9, 0x413bd7, 0x413bf7, 0x413c36, 0x413c77):
        phase.uc.hook_add(UC_HOOK_CODE, observe_grade_return,
                          begin=return_site, end=return_site)
    phase.icd.hooks[0x4161b0] = request_weight
    _, error = phase.init()
    assert error is None, error
    # This captured request already passed scheduler aging and admission in
    # World. Supply its recorded retail weight to the native initializer rather
    # than recalculating it from this standalone emulator's fresh queue.
    assert phase.get(0x54) == weight, (tick, phase.get(0x54), weight)
    native_start = struct.unpack('<hh', phase.uc.mem_read(OBJ + 0x30, 4))
    assert native_start == (sx - fx // 2, sz - fz // 2), (
        tick, native_start, sx, sz, fx, fz)
    assert phase.get(0xb0) == attempt[9], (
        tick, 'initial distance', phase.get(0xb0), attempt[9])
    # The emulated unit record omits the retail heavy-floater multiplier on
    # minimum straight distance. Use the captured request's exact kernel input.
    phase.uc.mem_write(OBJ + 0xb8, struct.pack('<i', attempt[19]))
    native_costs = tuple(phase.get(offset) for offset in
                         (0xc0, 0xc4, 0xbc, 0xc8, 0xb4, 0xb8))
    assert native_costs == tuple(attempt[14:20]), (tick, native_costs, attempt[14:20])
    phase.uc.mem_write(OBJ + 0x165, struct.pack('<I', 10_000_000))
    phase.uc.mem_write(OBJ + 0x5c, struct.pack('<I', 1))
    _, error = phase.step()
    assert error is None, error
    completed = struct.unpack('<I', phase.uc.mem_read(phase.NAV + 0x10c, 4))[0] != 0
    if not completed:
        assert phase.phase() == 2, (tick, 'retail rejected captured live route')
        for search_step in range(100_000):
            value, error = phase.step()
            assert error is None, (tick, search_step, error)
            if value:
                _, error = phase.icd.call(0x414450, (0,), ecx=OBJ)
                assert error is None, error
                completed = True
                break
    assert completed, (tick, unit_id, sx, sz, goal_x, goal_z)
    count = struct.unpack('<I', phase.uc.mem_read(phase.NAV + 0x10c, 4))[0]
    words = struct.unpack('<' + 'h' * (count * 2),
                          phase.uc.mem_read(phase.NAV + 12, count * 4)) if count else ()
    search_cells = phase.uc.mem_read(phase.get(0x1c), width * height * 4)
    search_plane_hash = 14695981039346656037
    for offset in range(0, len(search_cells), 4):
        search_plane_hash = ((search_plane_hash ^ search_cells[offset]) *
                             1099511628211) & 0xffffffffffffffff
        search_plane_hash = ((search_plane_hash ^ search_cells[offset + 1]) *
                             1099511628211) & 0xffffffffffffffff
    return (list(zip(words[::2], words[1::2])), dispatch_calls[0],
            live_grade_calls[0], grade_results, search_plane_hash)


def replay_native_worker_repath(width, height, base_cached_grades,
                                dynamic_cached_grades, base_live_grade,
                                dynamic_live_grade, attempt, profile,
                                target_x, target_z, circle_radius, sea,
                                position, mission_backed=False,
                                retry_through_4e5150=False,
                                live_blocker_collision=None):
    """Run a map-backed replacement through retail's queued route worker.

    The baseline starts at the captured World replan boundary. Collision mode
    instead starts at WORLDSEED, installs and checks the initial route, then
    advances that same mission/controller/mover into the registered live
    blocker before submitting a retry. Both variants use the singleton 0x416430
    worker; the direct-bit case seeds the captured refusal flag.
    """
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESI, UC_X86_REG_ESP

    (_tick, _unit_id, sx, sz, goal_x, goal_z, heading, retry, weight, *_rest) = attempt
    (turn, fx, fz, road, water, flags, transport_dist, max_water, min_water,
     half_cell_ticks, heavy) = profile
    goal_cell = (goal_x - fx // 2, goal_z - fz // 2)
    continuous_seed = (live_blocker_collision.get('world_seed')
                       if live_blocker_collision and
                       live_blocker_collision.get('continuous_approach') else None)
    if continuous_seed:
        (approach_x, _approach_y, approach_z, approach_heading,
         approach_speed, _approach_base, _terrain_flags) = continuous_seed
        position = (approach_x, approach_z)
        heading = approach_heading
        attach_start = ((approach_x // 65536) // 16 - fx // 2,
                        (approach_z // 65536) // 16 - fz // 2)
    else:
        approach_speed = None
        attach_start = (sx - fx // 2, sz - fz // 2)
    phase = Phase(width, height)
    native_live = None
    if not mission_backed:
        unit = phase.unit(sx - fx // 2, sz - fz // 2)
    assert phase.construct() is None
    if mission_backed:
        from probe_transport_surface_unload_callbacks import SurfaceUnload
        native_live = SurfaceUnload(placement_result=1, real_mission_removal=True,
            icd=phase.icd, game=GS, freeze_hooks=False)
        unit, mover, type_address = (native_live.carrier, native_live.mover,
                                     native_live.kind)
        nav, controller = native_live.nav, native_live.controller
        phase.uc.mem_write(unit + 2, struct.pack('<H', 1))
        native_live._nextController = controller
        phase.attach_live_request(unit, mover, nav, controller,
                                  attach_start, (fx, fz))
    else:
        phase.plant_request(unit, (sx - fx // 2, sz - fz // 2), goal_cell)
        mover = struct.unpack('<I', phase.uc.mem_read(unit + 8, 4))[0]
        nav, controller, type_address = phase.NAV, phase.HANDLE, TYPE
    phase.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
    phase.uc.mem_write(unit + 0x68, struct.pack('<iii', position[0],
                                                sea * 65536, position[1]))
    phase.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
    phase.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
    phase.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
    phase.uc.mem_write(type_address + 0x18e, struct.pack('<H', turn))
    phase.uc.mem_write(type_address + 0x172, struct.pack('<i', road))
    phase.uc.mem_write(type_address + 0x260, struct.pack('<I', 0x80000 if heavy else 0))
    phase.uc.mem_write(type_address + 0x16e, struct.pack('<i', water))
    phase.uc.mem_write(type_address + 0x192, struct.pack('<hh', max_water, min_water))
    phase.uc.mem_write(type_address + 0x249, bytes([half_cell_ticks]))
    phase.uc.mem_write(OBJ + 0x1ad, struct.pack('<I', retry))
    phase.uc.mem_write(phase.GRID + 4, struct.pack('<hh', fx, fz))
    phase.uc.mem_write(OBJ + 0xb8, struct.pack('<i', attempt[19]))
    phase.set_grade_plane(base_cached_grades)

    if native_live:
        phase.uc.mem_write(type_address + 0x23e, struct.pack('<H', transport_dist))
        phase.uc.mem_write(type_address + 0x264, struct.pack('<I', 0x200))
        phase.uc.mem_write(type_address + 0x14a, struct.pack('<I', 1 << 16))
        phase.uc.mem_write(type_address + 0x24b, b'\0')
        phase.uc.mem_write(type_address + 0x226, struct.pack('<h', 100))
        phase.uc.mem_write(native_live.owner + 0x8c,
                           struct.pack('<II', width // 2, height // 2))
        phase.uc.mem_write(native_live.mission + 0x22,
                           struct.pack('<i', target_x * 65536))
        phase.uc.mem_write(native_live.mission + 0x26, bytes(4))
        phase.uc.mem_write(native_live.mission + 0x2a,
                           struct.pack('<i', target_z * 65536))
        native_live.put(unit + 0xc4, native_live.mission + 0x12)
        native_live.put(native_live.passenger + 0xc4, native_live.mission + 0x12)
        first_slot = unit - 0x138
        first_page = first_slot & ~0xfff
        phase.uc.mem_map(first_page, 0x1000)
        phase.uc.mem_write(first_slot, bytes(0x138))
        native_live.put(native_live.owner + 0x74, first_slot)
        native_live.put(native_live.owner + 0x78, first_slot + 3 * 0x138)
    else:
        _, error = phase.icd.call(0x4e2500,
            (controller + 0x1000, target_x * 65536, target_z * 65536,
             circle_radius), ecx=controller)
        assert error is None, error
    if not native_live:
        native_goal = tuple(struct.unpack('<hh', phase.uc.mem_read(controller + 8, 4)))
        native_radius, radius_squared = struct.unpack('<ii',
            phase.uc.mem_read(controller + 0x0c, 8))
    else:
        native_goal = native_radius = radius_squared = None
    expected_radius_squared = int(circle_radius * circle_radius / 256 + 0.5)
    if not native_live:
        assert (native_goal, native_radius, radius_squared) == (
            goal_cell, circle_radius, expected_radius_squared), (
                native_goal, native_radius, radius_squared, goal_cell,
                circle_radius, expected_radius_squared)

    blocker_active = [False]
    active_live_grade = [base_live_grade]
    active_grade_footprint = [(fx, fz)]

    def query_live_grade(_uc, args):
        _who, world_x, _world_y, world_z = struct.unpack(
            '<Iiii', phase.uc.mem_read(args, 16))
        grade_fx, grade_fz = active_grade_footprint[0]
        x = ((world_x >> 19) - grade_fx) // 2
        z = ((world_z >> 19) - grade_fz) // 2
        if not (0 <= x < width and 0 <= z < height):
            return 4, 0
        return 4, active_live_grade[0](x, z)

    def no_gate(_uc, _args):
        return 0, 0

    def visible(_uc, _args):
        return 4, 1

    def no_special_body(_uc, _args):
        return 2, 0

    pending = [False]
    # The path scheduler is a fresh emulated singleton. Keep its clock in the
    # same zero-based range as the other worker probes; only coordinates and
    # movement profile come from the later captured World boundary.
    current_tick = [0]
    requests = []
    deliveries = []
    worker_events = []

    def put(address, value):
        phase.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def read(address):
        return struct.unpack('<I', phase.uc.mem_read(address, 4))[0]

    def lookup(_uc, _args):
        worker_events.append(('lookup', current_tick[0], pending[0]))
        return 0, nav if pending[0] else 0

    def enqueue(_uc, args):
        requests.append((current_tick[0], struct.unpack('<I',
            phase.uc.mem_read(args, 4))[0]))
        pending[0] = True
        return 1, 0

    def finish(_uc, _args):
        worker_events.append(('finish', current_tick[0]))
        pending[0] = False
        return 1, 0

    def request_weight(_uc, args):
        address = struct.unpack('<I', phase.uc.mem_read(args, 4))[0]
        phase.uc.mem_write(address, struct.pack('<I', weight))
        return 1, address

    def copy_delivery(uc, _address, _size, _data):
        if uc.reg_read(UC_X86_REG_ESI) != nav:
            return
        count = read(nav + 0x10c)
        words = struct.unpack('<' + 'h' * (count * 2),
                              uc.mem_read(nav + 12, count * 4)) if count else ()
        deliveries.append(list(zip(words[::2], words[1::2])))

    def prepare(_uc, _args):
        worker_events.append(('prepare', current_tick[0]))
        return 2, 0

    def notify(_uc, _args):
        worker_events.append(('notify', current_tick[0]))
        return 1, 0

    dispatch_calls = [0]

    def observe_dispatch(_uc, _address, _size, _data):
        dispatch_calls[0] += 1

    phase.uc.hook_add(UC_HOOK_CODE, observe_dispatch,
                      begin=0x4139d0, end=0x4139d0)
    phase.icd.hooks[0x4e4f50] = enqueue
    phase.icd.hooks[0x4e1ee0] = prepare
    phase.icd.hooks[0x4e2470] = notify
    phase.icd.hooks[0x4e2060] = finish
    phase.icd.hooks[0x413c80] = visible
    phase.icd.hooks[0x409fe0] = no_gate
    phase.icd.hooks[0x4db640] = query_live_grade
    phase.icd.hooks[0x50e600] = no_special_body
    phase.icd.hooks[0x4161b0] = request_weight
    vtable = read(nav)
    phase.icd.hooks[read(vtable + 0x18)] = lookup
    phase.uc.hook_add(UC_HOOK_CODE, copy_delivery,
                      begin=0x4e4f05, end=0x4e4f05)

    # Configure the native singleton's one-player scan and route-search slot.
    config = GS + 0x600000
    put(0x62d558, config)
    put(config, GS + 0x700000)
    put(config + 8, config + 0x100)
    put(config + 0x10c, 4)
    phase.uc.mem_write(GS + 0x3068, b'\x01\x00')
    owner = GS + 0x2404
    pool_first = unit - 0x138
    put(owner, 1)
    phase.uc.mem_write(owner + 0xea, b'\x01\x00')
    phase.uc.mem_write(owner + 0xe3, b'\x01')  # 4f6379: 5x path-budget class
    put(owner + 0x74, pool_first)
    put(owner + 0x78, pool_first + 3 * 0x138)
    put(OBJ + 0x115, pool_first)
    put(OBJ + 0x225, 12000)
    put(GS + 0x19e70, OBJ)

    def set_destination(game_tick):
        current_tick[0] = game_tick
        _, error = phase.icd.call(0x4e2500,
            (native_live.mission if native_live else controller + 0x1000,
             target_x * 65536, target_z * 65536,
             circle_radius), ecx=controller)
        assert error is None, ('circle controller update', error)
        _, error = phase.icd.call(0x4e54e0, (controller,), ecx=nav)
        assert error is None, ('native route request', error)

    def run_until_delivery(delivery_count, start_tick):
        if requests and start_tick < requests[-1][0]:
            raise AssertionError(('native route worker clock would precede its newest request',
                                  start_tick, requests[-1]))
        for game_tick in range(start_tick, start_tick + 500):
            current_tick[0] = game_tick
            put(GS + 0x19f44, game_tick)
            put(0x634674, int(pending[0]))
            _, error = phase.icd.call(0x416430, (1,), ecx=OBJ)
            assert error is None, ('retail route worker', game_tick, error)
            if len(deliveries) >= delivery_count and not pending[0]:
                return game_tick
        raise AssertionError(('retail route worker did not deliver', delivery_count,
                              requests, deliveries, read(OBJ + 0x5c), pending[0],
                              worker_events[-20:]))

    first_request = len(requests)
    if native_live:
        initial = native_live.dispatch(0)
        assert initial[0:3] == (1, 1, 0x701), initial
        controller = read(nav + 4)
        assert controller == native_live.controller
        expected_goal = (goal_cell, circle_radius, expected_radius_squared)
        native_goal = tuple(struct.unpack('<hh', phase.uc.mem_read(controller + 8, 4)))
        native_radius, radius_squared = struct.unpack('<ii',
            phase.uc.mem_read(controller + 0x0c, 8))
        assert (native_goal, native_radius, radius_squared) == expected_goal, (
            native_goal, native_radius, radius_squared, expected_goal)
        assert native_live.get(unit + 0x60) == native_live.mission
        assert native_live.get(unit + 0xac) == native_live.passenger
        assert native_live.get(native_live.passenger + 0xa8) == unit
    else:
        set_destination(0)
    assert pending[0] and len(requests) > first_request, (pending, requests)
    delivered_at = run_until_delivery(1, 1)
    first_route = deliveries[0]
    assert first_route, ('empty initial route', first_route)
    if continuous_seed and live_blocker_collision.get('initial_route') is not None:
        expected_initial = live_blocker_collision['initial_route']
        assert first_route == expected_initial, (
            'continuous native approach did not retain the initial map-backed route',
            first_route, expected_initial)

    original_nav, original_controller = read(mover), read(nav + 4)
    blocker_active[0] = True
    active_live_grade[0] = dynamic_live_grade
    if not continuous_seed:
        phase.set_grade_plane(dynamic_cached_grades)
    second_request = len(requests)
    if retry_through_4e5150:
        retry_tick = delivered_at + 1
        current_tick[0] = retry_tick
        put(GS + 0x19f44, retry_tick)
        put(0x634674, 0)
        if live_blocker_collision is not None:
            data = live_blocker_collision
            map_data = data['map_data']
            map_width, map_height, map_sea, map_heights, map_features, feature_count = map_data
            assert (map_width, map_height, map_sea) == (width, height, sea)
            (seed_x, seed_y, seed_z, seed_heading, seed_speed, seed_base,
             terrain_flags) = data['world_seed']
            max_velocity, acceleration, braking, unit_turn, waterline = data['world_type']
            assert terrain_flags == 0x1000 and seed_base > 0
            class_record = data['water_profile']
            class_fx, class_fz, max_depth, min_depth, bad_max_depth, bad_min_depth, \
                max_slope, bad_slope, max_water_slope, bad_water_slope = struct.unpack(
                    '<6h4B', class_record)
            assert (class_fx, class_fz) == (fx, fz)

            # Build the same raw cell and sector planes used by the paired map
            # mover trace. This leaves the original 0x507d10/0x4dc800 code in
            # charge of checking the ship against a live Vertrans body.
            records = bytearray(width * height * 14)
            raw_grades = [0] * (width * height)
            for z in range(height):
                for x in range(width):
                    index = z * width + x
                    corners = (map_heights[index],
                               map_heights[z * width + min(x + 1, width - 1)],
                               map_heights[min(z + 1, height - 1) * width + x],
                               map_heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)])
                    low, high = min(corners), max(corners)
                    offset = index * 14
                    records[offset + 4:offset + 7] = bytes((map_heights[index], high, low))
                    struct.pack_into('<H', records, offset + 8, map_features[index])
                    if low < sea - max_depth or high > sea - min_depth:
                        grade = 0
                    else:
                        slope = high - low
                        hard = max_water_slope if low < sea else max_slope
                        soft = bad_water_slope if low < sea else bad_slope
                        if slope > soft and slope > hard:
                            grade = 0
                        elif slope > soft or low < sea - bad_max_depth or high > sea - bad_min_depth:
                            grade = 4
                        else:
                            grade = 6
                    raw_grades[index] = grade
            phase.uc.mem_write(phase.cells_addr, bytes(records))
            phase.set_grade_plane(raw_grades)
            phase.uc.mem_write(GS + 0x19ef8, bytes((sea,)))

            feature_table = phase._alloc(max(320, feature_count * 320))
            phase.uc.mem_write(feature_table, bytes(max(320, feature_count * 320)))
            phase.uc.mem_write(GS + 0x19edc, struct.pack('<I', feature_table))
            phase.uc.mem_write(GS + 0x19ec0, struct.pack('<I', feature_count))

            sector_stride = (width + 7) // 8
            sector_rows = (height + 7) // 8
            sector_records = bytearray(sector_stride * sector_rows * 10)
            for sector_z in range(sector_rows):
                for sector_x in range(sector_stride):
                    block = [map_heights[z * width + x]
                             for z in range(sector_z * 8, min(height, sector_z * 8 + 8))
                             for x in range(sector_x * 8, min(width, sector_x * 8 + 8))]
                    sector_records[(sector_z * sector_stride + sector_x) * 10 + 1] = max(block)
            sector_grid = phase._alloc(len(sector_records))
            phase.uc.mem_write(sector_grid, bytes(sector_records))
            phase.uc.mem_write(GS + 0x19f18, struct.pack('<I', sector_grid))
            phase.uc.mem_write(GS + 0x19f1c, struct.pack('<I', sector_stride))
            phase.uc.mem_write(GS + 0x600000, struct.pack('<I', GS + 0x700000))
            phase.uc.mem_write(GS + 0x19f44, struct.pack('<I', retry_tick))

            # The carrier occupies real slot 1; the blocking Vertrans occupies
            # slot 2. Map a page below the carrier so retail's id*0x138 table
            # layout is valid without changing the mission's entity address.
            entity_base = unit - 0x138
            blocker_id = 2
            blocker = entity_base + blocker_id * 0x138
            blocker_nav = phase._alloc(0x180)
            phase.uc.mem_write(GS + 0x14e84, struct.pack('<II', entity_base,
                                                     entity_base + 4 * 0x138))
            phase.uc.mem_write(unit + 2, struct.pack('<H', 1))
            phase.uc.mem_write(blocker + 2, struct.pack('<H', blocker_id))
            phase.uc.mem_write(blocker + 8, struct.pack('<I', blocker_nav))
            phase.uc.mem_write(blocker + 0xb4, struct.pack('<I', type_address))
            phase.uc.mem_write(blocker + 0x130, struct.pack('<I', 0x1000000))
            blocker_x, blocker_z = data['blocker_position']
            blocker_origin_x = (blocker_x - (fx - 1) * 8 * 65536) // (16 * 65536)
            blocker_origin_z = (blocker_z - (fz - 1) * 8 * 65536) // (16 * 65536)
            phase.uc.mem_write(blocker + 0x68, struct.pack('<iii', blocker_x, seed_y, blocker_z))
            phase.uc.mem_write(blocker + 0x74, struct.pack('<hh', blocker_origin_x,
                                                       blocker_origin_z))
            phase.uc.mem_write(blocker + 0x78, struct.pack('<hh', fx, fz))
            phase.uc.mem_write(blocker + 0x7e, struct.pack('<H', data['blocker_heading']))
            phase.uc.mem_write(blocker_nav + 0x20, struct.pack('<i', 0))
            for z in range(max(0, blocker_origin_z), min(height, blocker_origin_z + fz)):
                for x in range(max(0, blocker_origin_x), min(width, blocker_origin_x + fx)):
                    phase.uc.mem_write(phase.cells_addr + (z * width + x) * 14,
                                   struct.pack('<H', blocker_id))

            phase.uc.mem_write(unit + 0x68, struct.pack('<iii', position[0], seed_y, position[1]))
            origin_x = (position[0] - (fx - 1) * 8 * 65536) // (16 * 65536)
            origin_z = (position[1] - (fz - 1) * 8 * 65536) // (16 * 65536)
            phase.uc.mem_write(unit + 0x74, struct.pack('<hh', origin_x, origin_z))
            phase.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
            phase.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
            phase.uc.mem_write(unit + 0x12b, struct.pack('<i', seed_base))
            phase.uc.mem_write(mover + 0x20, struct.pack('<i',
                seed_speed if continuous_seed else seed_base))
            phase.uc.mem_write(mover + 0x30, struct.pack('<I', retry_tick + 10000))
            phase.uc.mem_write(mover + 8, bytes(12))
            phase.uc.mem_write(mover + 0x14, bytes(12))
            phase.uc.mem_write(mover + 0x36, struct.pack('<H', flags | 1))
            phase.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
            phase.uc.mem_write(type_address + 0x162, struct.pack('<i', max_velocity))
            phase.uc.mem_write(type_address + 0x166, struct.pack('<i', braking))
            phase.uc.mem_write(type_address + 0x16a, struct.pack('<i', acceleration))
            phase.uc.mem_write(type_address + 0x18a, struct.pack('<I', phase.GRID))
            phase.uc.mem_write(type_address + 0x18e, struct.pack('<H', unit_turn))
            phase.uc.mem_write(type_address + 0x192, class_record[4:12])
            phase.uc.mem_write(type_address + 0x23c, bytes((max_slope, bad_slope,
                                                         max_water_slope, bad_water_slope)))
            phase.uc.mem_write(type_address + 0x248, bytes((waterline & 0xff,)))
            phase.uc.mem_write(type_address + 0x24a, b'\x01')
            phase.uc.mem_write(phase.GRID + 4, struct.pack('<hh', fx, fz))
            phase.uc.mem_write(phase.GRID + 8, class_record[4:])
            phase.icd.hooks[0x51ad20] = lambda _uc, _args: (1, 0)
            phase.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)

            collision_rows = []
            def observe_place(uc, address, _size, _data):
                if address == 0x507d10:
                    esp = uc.reg_read(UC_X86_REG_ESP)
                    collision_rows.append(tuple(struct.unpack('<5I',
                        uc.mem_read(esp + 4, 20))))
                elif address == 0x4daf96:
                    collision_rows.append(('result', uc.reg_read(UC_X86_REG_EAX)))
            phase.uc.hook_add(UC_HOOK_CODE, observe_place)
            hooks = phase.icd.hooks
            phase.icd.hooks = {address: hook for address, hook in hooks.items()
                           if address != 0x507d10}
            try:
                collision_step_limit = 5000 if continuous_seed else 4
                for collision_step in range(1, collision_step_limit + 1):
                    game_tick = retry_tick + collision_step
                    current_tick[0] = game_tick
                    put(GS + 0x19f44, game_tick)
                    value, error = phase.icd.call(0x4dc800, (unit,), ecx=mover)
                    assert error is None, ('native carrier collision mover',
                                           collision_step, error)
                    value, error = phase.icd.call(0x51b2a0, (unit,), ecx=mover)
                    assert error is None, ('native carrier position commit',
                                           collision_step, error)
                    move_flags = struct.unpack('<H', phase.uc.mem_read(mover + 0x36, 2))[0]
                    if move_flags & 4:
                        break
            finally:
                phase.icd.hooks = hooks
            assert move_flags & 4, ('native 0x4dc800 did not produce repeated live-body refusal',
                                     collision_rows,
                                     struct.unpack('<iii', phase.uc.mem_read(unit + 0x68, 12)),
                                     struct.unpack('<iii', phase.uc.mem_read(blocker + 0x68, 12)),
                                     hex(move_flags))
            if continuous_seed:
                native_position = struct.unpack('<iii', phase.uc.mem_read(unit + 0x68, 12))
                expected_position = live_blocker_collision.get('world_repath_position')
                if expected_position is not None:
                    assert (native_position[0], native_position[2]) == expected_position, (
                        'continuous native collision did not reach the captured World replan position',
                        native_position, expected_position)
                phase.set_grade_plane(dynamic_cached_grades)
            _, error = phase.icd.call(0x4e5150, (), ecx=nav)
            assert error is None, ('native 0x4e5150 repeated-block retry', error)
            assert pending[0] and len(requests) > second_request, (
                'native mover refusal did not reach 0x4e5150/0x4e4f50',
                requests, pending[0], hex(move_flags), collision_rows)
            print(f'  Retail 0x4dc800 produced mover refusal bit 4 after '
                  f'{collision_step} live-blocker updates and 0x4e5150 enqueued '
                  f'the mission retry; placement observations={collision_rows[-8:]}.')
        else:
            # The captured World retry follows two consecutive failed footprint
            # placements. Keep this explicit-state control as a diagnostic fallback.
            mover_flags = struct.unpack('<H', phase.uc.mem_read(mover + 0x36, 2))[0]
            phase.uc.mem_write(mover + 0x36, struct.pack('<H', mover_flags | 4))
            _, error = phase.icd.call(0x4e5150, (), ecx=nav)
            assert error is None, ('native 0x4e5150 repeated-block retry', error)
    else:
        set_destination(delivered_at + 1)
    assert pending[0] and len(requests) > second_request, (pending, requests)
    # The collision-derived branch advances the retail mover for two ticks
    # after the first route is delivered. Do not rewind the singleton clock to
    # the direct-bit control's earlier delivery window when running that retry.
    retry_worker_tick = max(delivered_at + 2, current_tick[0] + 1)
    run_until_delivery(2, retry_worker_tick)
    second_route = deliveries[1]
    assert second_route, ('empty retry route', first_route, second_route)
    if live_blocker_collision is None:
        assert second_route != first_route, (first_route, second_route)
    assert dispatch_calls[0] > 0, 'retail worker did not use the original grade dispatcher'
    assert read(mover) == original_nav == nav
    assert read(nav + 4) == original_controller == controller
    assert read(nav + 0x10c) == len(second_route), (read(nav + 0x10c), second_route)
    if native_live:
        assert native_live.get(unit + 0x60) == native_live.mission
        assert native_live.get(unit + 0xac) == native_live.passenger
        assert native_live.get(native_live.passenger + 0xa8) == unit
    end = second_route[-1]
    assert (end[0] - target_x) ** 2 + (end[1] - target_z) ** 2 <= circle_radius ** 2, second_route
    return second_route, len(requests), len(deliveries)


def check_route(world_binary, retail_root, map_name, start_cell, target_cell, footprint,
                carrier=None, passenger=None, crusades=False, native_map_grades=False,
                hpitool='build/hpitool', native_map_mover_steps=0,
                native_live_unload=False, terrain_scan_after=None,
                native_cargo_count=1,
                shore_blocker=False, live_route_blocker_steps=0,
                native_worker_repath=False, native_worker_mission_repath=False,
                always_on_route_search=False, native_worker_mission_retry=False,
                native_worker_mission_collision=False,
                probe_native_occupancy=False, native_detach_second=False,
                native_exploration=False, cargo_sight=None):
    if native_exploration and terrain_scan_after is None:
        raise ValueError('--native-exploration requires --terrain-scan-after')
    if (map_name.lower() == 'cairbray coast landing' and terrain_scan_after is not None
            and not native_exploration):
        raise ValueError('Cairbray live scans require --native-exploration: its forward '
                         'probes reach cells revealed during the trace')
    if native_live_unload and (not carrier or not native_map_mover_steps):
        raise ValueError('--native-live-unload requires a carrier and map mover steps')
    if not 1 <= native_cargo_count <= 16:
        raise ValueError('--native-cargo-count must be 1..16')
    if native_cargo_count > 1 and not native_live_unload:
        raise ValueError('--native-cargo-count above one requires --native-live-unload')
    if native_cargo_count > 1 and shore_blocker:
        raise ValueError('multi-cargo native unload is incompatible with --shore-blocker')
    if probe_native_occupancy and (not native_live_unload or native_cargo_count < 2):
        raise ValueError('--probe-native-occupancy requires a multi-cargo native live unload')
    if native_detach_second and not probe_native_occupancy:
        raise ValueError('--native-detach-second requires --probe-native-occupancy')
    native_live_profiles = {
        'lake lokken': {('vertrans', 'araarch'), ('verscout', 'araarch'),
                        ('verman', 'araarch'), ('aratrans', 'araarch'),
                        ('creiron', 'araarch'), ('arawar', 'araarch'),
                        ('crester', 'araarch'), ('npcbotl', 'araarch'),
                        ('npcrixx', 'araarch'), ('verharp', 'araarch')},
        'per mare per terras': {('vertrans', 'araarch')},
        'sea dragon spine': {('vertrans', 'araarch'), ('arawar', 'araarch')},
        # Cairbray's live scan reaches cells World explores during the trace;
        # it requires --native-exploration when the scan is live.
        'cairbray coast landing': {('vertrans', 'araarch')},
    }
    if native_live_unload and (not carrier or not passenger or
            (carrier.lower(), passenger.lower()) not in
            native_live_profiles.get(map_name.lower(), set())):
        raise ValueError('--native-live-unload currently checks Lake Lokken '
                         'Vertrans/VerScout/VerMan/Aratrans/Creiron/Arawar/Crester/'
                         'NpcBotl/NpcRixx/VerHarp with Araarch, and '
                         'Vertrans/Araarch plus Arawar/Araarch on Sea Dragon Spine')
    if terrain_scan_after is not None and not native_live_unload:
        raise ValueError('--terrain-scan-after requires --native-live-unload')
    if shore_blocker and (not native_live_unload or not carrier):
        raise ValueError('--shore-blocker requires an asset-backed --native-live-unload trace')
    if live_route_blocker_steps and (not carrier or not native_map_grades or
            native_map_mover_steps or native_live_unload or shore_blocker):
        raise ValueError('--live-route-blocker-steps requires an asset-backed route trace without the paired fixed-route mover')
    if live_route_blocker_steps and (map_name.lower() != 'lake lokken' or
            carrier.lower() != 'vertrans' or passenger.lower() != 'araarch'):
        raise ValueError('--live-route-blocker-steps currently checks Lake Lokken Vertrans/Araarch')
    if native_worker_repath and not live_route_blocker_steps:
        raise ValueError('--native-worker-repath requires --live-route-blocker-steps')
    if native_worker_mission_repath and not live_route_blocker_steps:
        raise ValueError('--native-worker-mission-repath requires --live-route-blocker-steps')
    if native_worker_mission_retry and not live_route_blocker_steps:
        raise ValueError('--native-worker-mission-retry requires --live-route-blocker-steps')
    if native_worker_mission_retry and always_on_route_search:
        raise ValueError('--native-worker-mission-retry requires the blocked, non-always-on fixture')
    if native_worker_mission_collision and not native_worker_mission_retry:
        raise ValueError('--native-worker-mission-collision requires --native-worker-mission-retry')
    if always_on_route_search and not live_route_blocker_steps:
        raise ValueError('--always-on-route-search requires --live-route-blocker-steps')
    env = os.environ.copy()
    env['TAK_DUMP_GRADE_PLANE'] = '1'
    if terrain_scan_after is not None:
        env['TAK_MAP_SURFACE_SCAN_AFTER'] = str(terrain_scan_after)
    if shore_blocker:
        env['TAK_MAP_SURFACE_BLOCK_SHORE'] = '1'
    if native_exploration:
        env['TAK_MAP_SURFACE_EXPLORATION'] = '1'
    if cargo_sight is not None:
        if not native_exploration:
            raise ValueError('--cargo-sight requires --native-exploration')
        env['TAK_MAP_SURFACE_CARGO_SIGHT'] = str(cargo_sight)
    if live_route_blocker_steps:
        env['TAK_MAP_SURFACE_ROUTE_BLOCKER'] = '1'
        env['TAK_MAP_SURFACE_SCAN_AFTER'] = '1'
        env['TAK_MAP_SURFACE_STEPS'] = str(live_route_blocker_steps)
    if always_on_route_search:
        env['TAK_MAP_SURFACE_ROUTE_ALWAYS_ON'] = '1'
    if carrier:
        env['TAK_DUMP_ATTEMPT_PLANES'] = '1'
        env['TAK_DUMP_ROUTE_ATTEMPT'] = '1'
        command = [world_binary, '--surface-unload-map-route-type', retail_root,
                   map_name, carrier, passenger, str(start_cell[0]), str(start_cell[1]),
                   str(target_cell[0]), str(target_cell[1]), str(int(crusades))]
    else:
        command = [world_binary, '--surface-unload-map-route', retail_root, map_name,
                   str(start_cell[0]), str(start_cell[1]), str(target_cell[0]),
                   str(target_cell[1]), str(footprint)]
    if native_map_mover_steps:
        env['TAK_MAP_SURFACE_STEPS'] = str(native_map_mover_steps)
    world = subprocess.run(command, check=True, capture_output=True, text=True, env=env)
    stderr = world.stderr.splitlines()
    profile = next(line for line in stderr if line.startswith('COSTPROFILE ')).split()
    turn, fx, fz, road, water, flags, cost_heavy, heading = map(int, profile[1:])
    completed_attempt = None
    if carrier:
        profiles = {}
        for line in stderr:
            if line.startswith('ATTEMPTPROFILE '):
                v = list(map(int, line.split()[1:]))
                tick, unit_id, attempt_turn, attempt_fx, attempt_fz, attempt_road, \
                    attempt_water, attempt_flags, transport_dist, max_water, min_water, \
                    half_cell_ticks, attempt_heavy, _last_retry = v
                profiles[(tick, unit_id)] = (attempt_turn, attempt_fx, attempt_fz,
                    attempt_road, attempt_water, attempt_flags, transport_dist,
                    max_water, min_water, half_cell_ticks, attempt_heavy)
        attempts = [(i, list(map(int, line.split()[1:])))
                    for i, line in enumerate(stderr)
                    if line.startswith('ATTEMPTPLANE ')]
        completed_attempt = next(list(map(int, line.split()[1:])) for line in stderr
                                 if line.startswith('WORLDATTEMPT '))
        (attempt_tick, attempt_unit, sx, sz, attempt_goal_x, attempt_goal_z,
         heading, retry, weight, initial_distance, partial_distance, phase,
         endpoint, route_flags, ground_cost, road_cost, slope_cost, traffic_cost,
         short_turn_cost, min_straight_cost, attempt_heavy, raw_count) = completed_attempt
        matching = [(i, h) for i, h in attempts
                    if h[0] == attempt_tick and h[1] == attempt_unit and
                    (h[2], h[3], h[4], h[5]) == (sx, sz, retry, heading)]
        assert len(matching) == 1, (completed_attempt, [h for _, h in attempts])
        header_index, plane = matching[0]
        (plane_tick, plane_unit, plane_sx, plane_sz, plane_retry, plane_heading,
         plane_weight, traffic_radius, width, height, grade_fx, grade_fz) = plane
        assert (plane_tick, plane_unit, plane_sx, plane_sz, plane_retry, plane_heading,
                plane_weight) == (attempt_tick, attempt_unit, sx, sz, retry, heading, weight)
        type_profile = profiles[(attempt_tick, attempt_unit)]
        (turn, fx, fz, road, water, flags, _transport_dist, max_water, min_water,
         half_cell_ticks, attempt_heavy_profile) = type_profile
        assert (fx, fz, attempt_heavy_profile) == (grade_fx, grade_fz, attempt_heavy)
        assert traffic_radius == 50 // half_cell_ticks, \
            (traffic_radius, half_cell_ticks)
        expected_costs = expected_path_costs(
            turn, fx, road, water, flags, bool(cost_heavy))
        assert (ground_cost, road_cost, slope_cost, traffic_cost, short_turn_cost,
                min_straight_cost) == expected_costs, completed_attempt
        grades = [int(value) for line in stderr[header_index + 1:header_index + 1 + height]
                  for value in line.split()]
        assert len(grades) == width * height
        world_raw_rows = [tuple(map(int, line.split()[2:])) for line in stderr
                          if line.startswith('WORLDRAW ')]
        # The live-route fixture can emit additional completed attempts after
        # this initial route. Keep the first completion paired with the
        # initial ATTEMPTPLANE; later WORLDRAW rows are parsed separately below.
        world_raw = world_raw_rows[:raw_count]
        assert len(world_raw) == raw_count and world_raw[0] == (sx, sz), \
            (world_raw, completed_attempt)
    else:
        header_index = next(i for i, line in enumerate(stderr) if line.startswith('GRADEPLANE '))
        width, height, grade_fx, grade_fz, retry, sx, sz, tick, grade_heading = map(
            int, stderr[header_index].split()[1:])
        assert (grade_fx, grade_fz, retry, grade_heading) == (fx, fz, 0, heading)
        grades = [int(value) for line in stderr[header_index + 1:header_index + 1 + height]
                  for value in line.split()]
        assert len(grades) == width * height
        assert not any(line.startswith('GRADECHANGE ') for line in
                       stderr[header_index + 1 + height:]), \
            'the effective grade plane changed during this search; static comparison is invalid'

    transport_profile = next((line for line in stderr
                              if line.startswith('TRANSPORTPROFILE ')), None)
    if transport_profile:
        transport_dist, max_water, min_water, heavy = map(
            int, transport_profile.split()[1:])
        assert transport_dist > 34, transport_profile
        assert heavy == cost_heavy, (transport_profile, profile)
        circle_radius = transport_dist - 34
    else:
        max_water, min_water, heavy = 10000, 13, 1
        circle_radius = 116

    stdout = world.stdout.splitlines()
    map_header = next(line for line in stdout if line.startswith('MAPROUTE ')).split()
    map_width, map_height, sea, map_foot, start_x, start_z, target_x, target_z, _unit_id = \
        map(int, map_header[1:])
    if carrier:
        assert (map_width, map_height, map_foot, fx, fz) == (width, height, fx, fx, fz)
    else:
        assert (map_width, map_height, map_foot, fx, fz) == \
            (width, height, footprint, footprint, footprint)
    route_index = next(i for i, line in enumerate(stdout) if line.startswith('ROUTE '))
    route_header = list(map(int, stdout[route_index].split()[1:]))
    route_tick, _, _, anchor_x, anchor_z, route_count, completions, failures = route_header
    assert completions + failures > 0, route_header
    anchor = (anchor_x // 65536, anchor_z // 65536)
    # ROUTE includes its start/anchor entry in route_count but the fixture emits
    # only actual order waypoints. Optional mover diagnostics follow those
    # pairs, so select coordinate rows rather than consuming route_count lines.
    world_points_fixed = []
    for line in stdout[route_index + 1:]:
        fields = line.split()
        if len(fields) != 2:
            if world_points_fixed:
                break
            continue
        try:
            world_points_fixed.append(tuple(map(int, fields)))
        except ValueError:
            if world_points_fixed:
                break
    world_route = [(x // 65536, z // 65536) for x, z in world_points_fixed]
    # ROUTE includes the anchor/start entry in its reported count, while the
    # fixture prints only the following actual order waypoints.
    expected_emitted_points = route_count - 1
    assert len(world_route) == expected_emitted_points, (
        route_header, world_route, expected_emitted_points)
    world_steps = []
    world_seed = world_type = None
    if native_map_mover_steps:
        world_seed = tuple(map(int, next(line for line in stdout
                                         if line.startswith('WORLDSEED ')).split()[1:]))
        world_type = tuple(map(int, next(line for line in stdout
                                         if line.startswith('WORLDTYPE ')).split()[1:]))
        world_steps = [tuple(map(int, line.split()[1:])) for line in stdout
                       if line.startswith('WORLDSTEP ')]
        assert len(world_steps) == native_map_mover_steps, (
            len(world_steps), native_map_mover_steps)
    world_sight = None
    world_cargo_sight = None
    world_cargo_steps = {}
    world_cargo_first = None
    world_exploration_init = {}
    world_exploration_deltas = {}
    if native_exploration:
        world_sight = tuple(map(int, next(line for line in stdout
                                          if line.startswith('WORLDSIGHT ')).split()[1:]))
        cargo_line = next((line for line in stdout
                           if line.startswith('WORLDCARGOSIGHT ')), None)
        if cargo_line is not None:
            world_cargo_sight = tuple(map(int, cargo_line.split()[1:]))
        for line in stdout:
            if line.startswith('WORLDCARGO '):
                step, x, y, z, first = map(int, line.split()[1:])
                world_cargo_steps[step] = (x, y, z)
                world_cargo_first = bool(first)
        for line in stdout:
            if line.startswith('WORLDEXP_INIT '):
                x, z, value = map(int, line.split()[1:])
                world_exploration_init[(x, z)] = value
            elif line.startswith('WORLDEXP_DELTA '):
                step, x, z, value = map(int, line.split()[1:])
                world_exploration_deltas.setdefault(step, []).append((x, z, value))
    world_blockers = {}
    if shore_blocker:
        for line in stdout:
            if line.startswith('WORLD_BLOCKER '):
                row = tuple(map(int, line.split()[1:]))
                assert len(row) == 8, row
                world_blockers[row[0]] = row[1:]
        assert len(world_blockers) == native_map_mover_steps + 1, (
            len(world_blockers), native_map_mover_steps)
        assert sorted(world_blockers) == list(range(native_map_mover_steps + 1))
    if live_route_blocker_steps:
        route_blocker = tuple(map(int, next(line for line in stdout
            if line.startswith('WORLD_ROUTE_BLOCKER ')).split()[1:]))
        assert len(route_blocker) == 6, route_blocker
        blocker_id, blocker_x, blocker_z, initial_waypoints, blocker_fx, blocker_fz = route_blocker
        assert blocker_id > 0 and initial_waypoints > 0 and blocker_fx == fx and blocker_fz == fz, route_blocker
        if always_on_route_search:
            search_enabled = [tuple(map(int, line.split()[1:])) for line in stdout
                              if line.startswith('WORLD_ROUTE_BLOCK_THRESHOLD ')]
            assert len(search_enabled) <= 1, search_enabled
            assert not any(line.startswith('WORLD_ROUTE_SEARCH_ENABLED ')
                           for line in stdout), 'always-on route service was disabled'
        else:
            search_enabled = [tuple(map(int, line.split()[1:])) for line in stdout
                              if line.startswith('WORLD_ROUTE_SEARCH_ENABLED ')]
            assert len(search_enabled) == 1 and search_enabled[0][1] >= 2, search_enabled
        repaths = [tuple(map(int, line.split()[1:])) for line in stdout
                   if line.startswith('WORLD_REPATH ')]
        assert repaths and all(len(row) == 10 for row in repaths), repaths[:5]
        changed_repaths = [row for row in repaths if row[3] and
                           (always_on_route_search or row[7] >= 2) and
                           row[8] == 1 and row[9] == 1]
        assert changed_repaths and (always_on_route_search or
                                    changed_repaths[0][0] >= search_enabled[0][0]), (
            'World did not install a changed route while the boat was body-blocked '
            'with cargo retained', search_enabled, repaths[:8])
        if native_worker_mission_retry:
            assert changed_repaths[0][7] >= 2, (
                'native 0x4e5150 retry must be seeded from the captured repeated-block state',
                changed_repaths[0])
        route_bodies = {row[0]: row[1:] for row in
            (tuple(map(int, line.split()[1:])) for line in stdout
             if line.startswith('WORLD_ROUTE_BODY '))}
        assert len(route_bodies) == live_route_blocker_steps, len(route_bodies)
        blocked_state = route_bodies[changed_repaths[0][0]]
        assert blocked_state[0] == blocker_x and blocked_state[1] == blocker_z, (
            'the blocker moved before the changed route was installed',
            route_blocker, changed_repaths[0], blocked_state)
        release_rows = [tuple(map(int, line.split()[1:])) for line in stdout
                        if line.startswith('WORLD_ROUTE_RELEASE ')]
        assert len(release_rows) == 1 and len(release_rows[0]) == 9, release_rows
        (route_release_step, carrier_x, carrier_z, passenger_x, passenger_z,
         carrier_water, carrier_in_circle, remaining_cargo, passenger_attached) = release_rows[0]
        assert route_release_step <= live_route_blocker_steps, route_release_step
        landing_dx = passenger_x - target_x * 65536
        landing_dz = passenger_z - target_z * 65536
        assert landing_dx * landing_dx + landing_dz * landing_dz <= (8 * 65536) ** 2, release_rows[0]
        assert (carrier_water, carrier_in_circle, remaining_cargo, passenger_attached) == (1, 1, 0, 0), release_rows[0]
        released_blocker = route_bodies[route_release_step]
        assert released_blocker[4] == 1, released_blocker
        blocker_moved = ((released_blocker[0] - blocker_x) ** 2 +
                         (released_blocker[1] - blocker_z) ** 2) > (64 * 65536) ** 2
        assert blocker_moved, (route_blocker, released_blocker)
        changed_step = changed_repaths[0][0]
        changed_tick = route_tick + changed_step
        captured_attempts = []
        for line_index, line in enumerate(stderr):
            if not line.startswith('WORLDATTEMPT '):
                continue
            values = list(map(int, line.split()[1:]))
            raw_count = values[-1]
            raw_points = [tuple(map(int, row.split()[2:]))
                          for row in stderr[line_index + 1:line_index + 1 + raw_count]
                          if row.startswith('WORLDRAW ')]
            assert len(raw_points) == raw_count, (values, raw_points)
            captured_attempts.append((values, raw_points))
        dynamic_matches = [(values, points) for values, points in captured_attempts
                           if values[0] == changed_tick and values[1] == _unit_id]
        assert len(dynamic_matches) == 1, (changed_step, route_tick,
                                           [row[0][:8] for row in captured_attempts])
        dynamic_attempt, dynamic_world_raw = dynamic_matches[0]
        dynamic_key = (dynamic_attempt[1], dynamic_attempt[2], dynamic_attempt[3],
                       dynamic_attempt[7], dynamic_attempt[6])
        dynamic_planes = [(index, header) for index, header in attempts
                          if (header[1], header[2], header[3],
                              header[4], header[5]) == dynamic_key and
                          header[0] <= dynamic_attempt[0]]
        if dynamic_planes:
            latest_plane_tick = max(header[0] for _, header in dynamic_planes)
            dynamic_planes = [(index, header) for index, header in dynamic_planes
                              if header[0] == latest_plane_tick]
        assert len(dynamic_planes) == 1, (dynamic_attempt, dynamic_planes)
        plane_index, plane_header = dynamic_planes[0]
        plane_width, plane_height = plane_header[8:10]
        dynamic_grades = [int(value)
                          for row in stderr[plane_index + 1:plane_index + 1 + plane_height]
                          for value in row.split()]
        assert len(dynamic_grades) == plane_width * plane_height
        blocker_cell = ((blocker_x // 65536 - (fx - 1) * 8) // 16,
                        (blocker_z // 65536 - (fz - 1) * 8) // 16)
        bx, bz = blocker_cell
        assert grades[bz * width + bx] >= 6 and dynamic_grades[bz * width + bx] in (0, 2), (
            'the live boat blocker was not added to the captured route-grade plane',
            blocker_cell, grades[bz * width + bx], dynamic_grades[bz * width + bx])
        route_map = parse_tnt(cat(hpitool, Path(retail_root), 'maps.hpi',
                                  f'Maps/{map_name}.tnt'))
        _, water_profile, native_fx, native_fz = native_water_profile(hpitool, retail_root)
        assert (native_fx, native_fz) == (fx, fz), (native_fx, native_fz, fx, fz)
        native_map_grade = native_grade_reader(route_map, water_profile)
        block_rect = (bx, bz, blocker_fx, blocker_fz)

        def overlaps(left, top, cells_wide, cells_high):
            return (left < bx + blocker_fx and bx < left + cells_wide and
                    top < bz + blocker_fz and bz < top + cells_high)

        grade_checks = 0
        grade_mismatches = []
        for z in range(max(0, bz - 8), min(height, bz + blocker_fz + 8)):
            for x in range(max(0, bx - 8), min(width, bx + blocker_fx + 8)):
                index = z * width + x
                # Grade 5 is World's visibility fallback, not a terrain score
                # from retail's native 0x508cd0 routine.
                if grades[index] == 5:
                    continue
                base = native_map_grade(x, z)
                if base != grades[index]:
                    grade_mismatches.append((x, z, grades[index], base, 'static'))
                    continue
                if overlaps(x, z, fx, fz):
                    if dynamic_grades[index] not in (0, 2):
                        grade_mismatches.append((x, z, dynamic_grades[index],
                                                 (0, 2), 'blocker'))
                    continue
                elif (overlaps(x - 1, z - 1, fx + 1, 1) or
                      overlaps(x + fx, z - 1, 1, fz + 1) or
                      overlaps(x, z + fz, fx + 1, 1) or
                      overlaps(x - 1, z, 1, fz + 1)):
                    expected = min(base, 4)
                else:
                    expected = base
                grade_checks += 1
                if dynamic_grades[index] != expected:
                    grade_mismatches.append((x, z, dynamic_grades[index], expected, 'blocker'))
        assert not grade_mismatches, ('native map/blocker grade-plane mismatch',
                                      grade_mismatches[:20])
        assert grade_checks > 0, ('no independently checked blocker grade cells', block_rect)

        live_grade_reads = []
        live_grade_mismatches = []

        def native_live_grade(x, z):
            index = z * width + x
            # Retail's 4db640 rechecks a grade-2 cached cell against terrain
            # and live bodies. A mobile occupant that cannot be followed gives
            # grade 2; the cached occupancy plane may independently mark its
            # footprint as 0 or retain the grade-2 refresh sentinel.
            value = 2 if overlaps(x, z, fx, fz) else native_map_grade(x, z)
            live_grade_reads.append((x, z, value))
            if dynamic_grades[index] != 2 or value != 2:
                live_grade_mismatches.append(
                    (x, z, dynamic_grades[index], value))
            return value

        dynamic_profile_matches = [(tick, profile) for (tick, unit_id), profile
                                   in profiles.items()
                                   if unit_id == dynamic_attempt[1] and
                                   tick <= dynamic_attempt[0]]
        assert dynamic_profile_matches, dynamic_attempt
        dynamic_profile_tick = max(tick for tick, _ in dynamic_profile_matches)
        dynamic_profile = next(profile for tick, profile in dynamic_profile_matches
                               if tick == dynamic_profile_tick)
        (dynamic_native_route, native_dispatch_calls, native_live_grade_calls,
         native_grade_results, native_search_plane_hash) = replay_native_attempt(
            plane_width, plane_height, dynamic_grades, native_live_grade,
            dynamic_attempt, dynamic_profile,
            target_x, target_z, circle_radius, sea,
            (changed_repaths[0][4], changed_repaths[0][5]))
        assert native_dispatch_calls > 0, 'retail route skipped original 0x4139d0'
        native_grade_mismatches = []
        for x, z, value, return_site in native_grade_results:
            expected = (dynamic_grades[z * width + x]
                        if 0 <= x < width and 0 <= z < height else 0)
            if value != expected:
                native_grade_mismatches.append(
                    (x, z, value, expected, return_site))
        assert not native_grade_mismatches, (
            'retail 0x4139d0 grade results differ from World attempt plane',
            native_grade_mismatches[:20])
        assert len(live_grade_reads) == native_live_grade_calls, (
            len(live_grade_reads), native_live_grade_calls)
        assert not live_grade_mismatches, (
            'retail live grade-2 refresh differs from the captured blocker plane',
            live_grade_mismatches[:20])
        world_search_rows = [line.split()[1:] for line in stderr
                             if line.startswith('WORLDSEARCH ')]
        dynamic_world_search = [row for row in world_search_rows
                                if int(row[0]) == dynamic_attempt[0] and
                                   int(row[1]) == dynamic_attempt[1] and
                                   int(row[2]) == dynamic_attempt[7]]
        assert len(dynamic_world_search) == 1, (dynamic_attempt, dynamic_world_search)
        world_search_plane_hash = int(dynamic_world_search[0][6], 16)
        dynamic_world_pixels = [(x * 16, z * 16) for x, z in dynamic_world_raw]
        common_prefix = 0
        for native_point, world_point in zip(dynamic_native_route, dynamic_world_pixels):
            if native_point != world_point:
                break
            common_prefix += 1
        if dynamic_native_route != dynamic_world_pixels:
            message = (
                'retail native search did not reproduce the live-blocker replacement route',
                dynamic_attempt, dynamic_native_route, dynamic_world_pixels,
                native_dispatch_calls, native_live_grade_calls,
                live_grade_reads, live_grade_mismatches[:20],
                native_grade_mismatches[:20], hex(world_search_plane_hash),
                hex(native_search_plane_hash))
            if not always_on_route_search and not native_worker_mission_retry:
                raise AssertionError(message)
            print('  Direct captured-attempt mismatch (continuing through worker):',
                  message)
        for route_name, route in (('retail', dynamic_native_route),
                                  ('World', dynamic_world_pixels)):
            dx, dz = route[-1][0] - target_x, route[-1][1] - target_z
            assert dx * dx + dz * dz <= circle_radius * circle_radius, (
                route_name, route[-1], (target_x, target_z), circle_radius)
        if native_worker_repath:
            worker_route, worker_request_count, worker_delivery_count = \
                replay_native_worker_repath(
                    plane_width, plane_height, grades, dynamic_grades,
                    native_map_grade, native_live_grade, dynamic_attempt,
                    dynamic_profile,
                    target_x, target_z, circle_radius, sea,
                    (changed_repaths[0][4], changed_repaths[0][5]))
            if not always_on_route_search:
                assert worker_route == dynamic_native_route, (
                    'retail route worker did not install the direct native dynamic route',
                    worker_route, dynamic_native_route, dynamic_attempt)
            assert worker_route == dynamic_world_pixels, (
                'retail route worker did not reproduce the captured World replacement',
                worker_route, dynamic_world_pixels, dynamic_attempt)
            assert not live_grade_mismatches, (
                'retail worker queried a grade that differs from the captured blocker plane',
                live_grade_mismatches[:20])
            print(f'  Retail 0x416430 worker delivered the blocked replacement on the '
                  f'same navigator/controller ({worker_delivery_count} deliveries from '
                  f'{worker_request_count} worker requests); the replacement changed '
                  f'the route and matches all {len(worker_route)} World waypoints.')
        if native_worker_mission_repath:
            mission_route, mission_request_count, mission_delivery_count = \
                replay_native_worker_repath(
                    plane_width, plane_height, grades, dynamic_grades,
                    native_map_grade, native_live_grade, dynamic_attempt,
                    dynamic_profile,
                    target_x, target_z, circle_radius, sea,
                    (changed_repaths[0][4], changed_repaths[0][5]),
                    mission_backed=True)
            if not always_on_route_search:
                assert mission_route == dynamic_native_route, (
                    'native sea-unload mission worker did not install the direct native dynamic route',
                    mission_route, dynamic_native_route, dynamic_attempt)
            assert mission_route == dynamic_world_pixels, (
                'native sea-unload mission worker did not reproduce the World replacement',
                mission_route, dynamic_world_pixels, dynamic_attempt)
            assert not live_grade_mismatches, (
                'native sea-unload worker queried a grade that differs from the blocker plane',
                live_grade_mismatches[:20])
            print(f'  Retail unload dispatcher kept carrier and Araarch attached through '
                  f'{mission_delivery_count} worker deliveries ({mission_request_count} '
                  f'requests); its live mission route replacement matches all '
                  f'{len(mission_route)} World waypoints.')
        if native_worker_mission_retry:
            collision_inputs = None
            if native_worker_mission_collision:
                collision_inputs = {
                    'continuous_approach': True,
                    'initial_route': [(x * 16 + (fx % 2) * 8,
                                       z * 16 + (fz % 2) * 8)
                                      for x, z in world_raw],
                    'world_repath_position': (changed_repaths[0][4],
                                              changed_repaths[0][5]),
                    'map_data': route_map,
                    'world_seed': tuple(map(int, next(line for line in stdout
                        if line.startswith('WORLDSEED ')).split()[1:])),
                    'world_type': tuple(map(int, next(line for line in stdout
                        if line.startswith('WORLDTYPE ')).split()[1:])),
                    'water_profile': water_profile,
                    'blocker_position': (blocked_state[0], blocked_state[1]),
                    'blocker_heading': blocked_state[3],
                }
            retry_route, retry_request_count, retry_delivery_count = \
                replay_native_worker_repath(
                    plane_width, plane_height, grades, dynamic_grades,
                    native_map_grade, native_live_grade, dynamic_attempt,
                    dynamic_profile,
                    target_x, target_z, circle_radius, sea,
                    (changed_repaths[0][4], changed_repaths[0][5]),
                    mission_backed=True, retry_through_4e5150=True,
                    live_blocker_collision=collision_inputs)
            if not native_worker_mission_collision:
                assert retry_route == dynamic_world_pixels, (
                    'native 0x4e5150 retry worker did not reproduce the captured World replacement',
                    retry_route, dynamic_world_pixels, dynamic_attempt)
            elif retry_route == dynamic_world_pixels:
                print('  Collision-derived repeated refusal delivered the captured '
                      'World replacement route.')
            else:
                print('  Collision-derived repeated refusal reached 0x4e5150 and '
                      'delivered a retry, but that request retained the existing '
                      f'route ({len(retry_route)} points vs {len(dynamic_world_pixels)} '
                      'captured World points); this fixture does not establish a '
                      'retail-vs-World path mismatch.')
            assert not live_grade_mismatches, (
                'native 0x4e5150 retry queried a grade that differs from the blocker plane',
                live_grade_mismatches[:20])
            assert not live_grade_reads, (
                'native 0x4e5150 retry unexpectedly needed a live 0x4db640 refresh',
                live_grade_reads)
            if not native_worker_mission_collision:
                print(f'  Retail 0x4e5150 consumed the World-captured repeated-block flag and '
                      f'enqueued the live mission retry; 0x416430 delivered the replacement on the '
                      f'same navigator/controller ({retry_delivery_count} deliveries from '
                      f'{retry_request_count} worker requests), matching all '
                      f'{len(retry_route)} World waypoints with no 0x4db640 refreshes.')
        print(f'  World hit a live map-backed boat blocker, installed a changed '
              f'route at physical step {changed_repaths[0][0]}, cleared the blocker, '
              f'and released Araarch at the selected shore on step {route_release_step}.')
        print(f'  Retail native search reproduced {common_prefix}/{len(dynamic_world_pixels)} '
              f'replacement-route points through {native_dispatch_calls} original 0x4139d0 calls '
              f'({native_live_grade_calls} live 0x4db640 refreshes); {grade_checks} '
              f'local grades match TNT terrain plus the blocker; the endpoint '
              f'is inside the same {circle_radius}px unload circle.')
    if transport_profile and world_route:
        dx = world_route[-1][0] - target_x
        dz = world_route[-1][1] - target_z
        assert dx * dx + dz * dz <= circle_radius * circle_radius, \
            (world_route[-1], (target_x, target_z), circle_radius)

    p = Phase(width, height)
    native_live = None
    native_placement_results = []
    placement_blocker_states = []
    blocked_placement_states = []
    blocked_cargo_held = []
    active_blocker_state = None
    native_passengers = []
    active_native_passenger = [None]
    native_occupancy_probe_state = None
    native_placement_trace_active = [False]
    native_real_placement_enabled = [False]
    native_placement_return_hooks = []
    native_occupancy_live_trace = []
    native_occupancy_probe_tick = [0]
    native_detach_native_entries = []
    if native_live_unload:
        tnt_data = cat(hpitool, Path(retail_root), 'maps.hpi',
                       f'Maps/{map_name}.tnt')
        live_map_data = parse_tnt(tnt_data)
        moveinfo_text = cat(hpitool, Path(retail_root), 'data.hpi',
                            'gamedata/moveinfo.tdf').decode('latin1')
        passenger_unit_path = f'units/{passenger}.fbi'
        if crusades:
            try:
                passenger_unit_path = f'unitscb/{passenger}.fbi'
                passenger_unit_text = cat(hpitool, Path(retail_root), 'data.hpi',
                                          passenger_unit_path).decode('latin1')
            except subprocess.CalledProcessError:
                passenger_unit_path = f'units/{passenger}.fbi'
                passenger_unit_text = cat(hpitool, Path(retail_root), 'data.hpi',
                                          passenger_unit_path).decode('latin1')
        else:
            passenger_unit_text = cat(hpitool, Path(retail_root), 'data.hpi',
                                      passenger_unit_path).decode('latin1')
        passenger_profile_fields = retail_unit_profile(
            hpitool, retail_root, passenger, crusades)
        passenger_profile = movement_profile(moveinfo_text, passenger_unit_text)
        carrier_profile = retail_unit_profile(hpitool, retail_root, carrier,
                                              crusades)
        capacity_count = int(float(carrier_profile.get('transportcapacity', '0')))
        capacity_size = int(float(carrier_profile.get('transportsizecapacity', '0')))
        capacity_passenger = int(float(carrier_profile.get('transportsize', '0')))
        passenger_size = int(float(passenger_profile_fields.get(
            'transportedsize', str(passenger_profile[0] * passenger_profile[1]))))
        if capacity_count <= 0 or capacity_size <= 0 or capacity_passenger <= 0:
            raise AssertionError(('carrier capacity profile', carrier, carrier_profile))
        max_cargo_count = (min(capacity_count, capacity_size // passenger_size)
                           if passenger_size <= capacity_passenger else 0)
        if native_cargo_count > max_cargo_count:
            raise ValueError((f'{native_cargo_count} passengers exceed the selected FBI capacity',
                              carrier, capacity_count, capacity_size,
                              capacity_passenger, passenger_size))
        capacity_inputs = [
            f'{passenger_size} {capacity_passenger} {count} {capacity_count} '
            f'{count * passenger_size} {capacity_size}'
            for count in range(max_cargo_count + 1)
        ]
        capacity_result = subprocess.run(
            [world_binary, '--capacity'], input='\n'.join(capacity_inputs) + '\n',
            check=True, capture_output=True, text=True)
        capacity_rows = [line.strip() for line in capacity_result.stdout.splitlines()]
        expected_capacity_rows = ['1'] * max_cargo_count + ['0']
        if capacity_rows != expected_capacity_rows:
            raise AssertionError(('FBI capacity boundary disagrees with production helper',
                                  carrier, capacity_inputs, capacity_rows,
                                  expected_capacity_rows))
        print(f'  Selected FBI capacity: {capacity_count} passengers, '
              f'{capacity_size} total size, {capacity_passenger} per passenger; '
              f'{passenger_size}-size {passenger} admits {max_cargo_count}; '
              f'production capacity helper rejects one more.')
        carrier_sight = int(carrier_profile.get('sightdistance', '0'))
        if carrier_sight <= 0:
            raise AssertionError(('carrier sight distance', carrier, carrier_profile))
        authored_transport_dist = int(float(
            carrier_profile.get('transportdistance', '0')))
        if transport_dist != authored_transport_dist:
            raise AssertionError(('World/native transport distance differs from selected FBI',
                                  transport_dist, authored_transport_dist, carrier,
                                  carrier_profile))
        authored_mover_type = (
            int(float(carrier_profile.get('maxvelocity', '0')) * 65536),
            int(float(carrier_profile.get('acceleration', '0')) * 65536),
            int(float(carrier_profile.get('brakerate', '0')) * 65536),
            int(float(carrier_profile.get('turnrate', '0'))),
            int(float(carrier_profile.get('waterline', '0'))))
        if native_map_mover_steps and world_type != authored_mover_type:
            raise AssertionError(('World mover inputs differ from selected FBI',
                                  world_type, authored_mover_type, carrier,
                                  carrier_profile))
        native_place = native_placement_oracle(live_map_data, passenger_profile)

        def checked_placement(args, call_number):
            result = native_place(args, call_number)
            native_placement_results.append((args, result))
            placement_blocker_states.append(active_blocker_state)
            if not result:
                blocked_placement_states.append(active_blocker_state)
                if native_live:
                    blocked_cargo_held.append(
                        native_live.get(native_live.carrier + 0xac) ==
                        active_native_passenger[0] and
                        native_live.get(active_native_passenger[0] + 0xa8) ==
                        native_live.carrier)
            return result

        from probe_transport_surface_unload_callbacks import SurfaceUnload
        native_live = SurfaceUnload(placement_result=checked_placement,
            real_mission_removal=True, icd=p.icd, game=GS, freeze_hooks=False)
        unit, mover, type_address = native_live.carrier, native_live.mover, native_live.kind
        native_passengers = [native_live.passenger]
        active_native_passenger[0] = native_live.passenger
        native_detach_events = []
        for index in range(1, native_cargo_count):
            extra = p._alloc(0x140)
            p.uc.mem_write(extra, bytes(0x140))
            # The placement oracle reserves entity id 2 for live blockers.
            # Give additional cargo distinct native ids so the placement
            # routine does not mistake a reserved passenger for itself.
            native_live.put(extra + 2, index + 2)
            native_live.put(extra + 0x130, 0x1000000)
            native_live.put(extra + 0xA8, unit)
            native_live.put(extra + 0xB4, type_address)
            native_passengers.append(extra)
        # Retail cargo is a linked list: +0xAC is the head, with owner and
        # next pointers at passenger +0xA8 and +0xB0.
        for index, cargo in enumerate(native_passengers):
            native_live.put(cargo + 0xB0,
                            native_passengers[index + 1]
                            if index + 1 < len(native_passengers) else 0)
        native_live.put(unit + 0xAC, native_passengers[0])

        def detach_native_passenger(uc, sp):
            cargo = active_native_passenger[0]
            args = struct.unpack('<5I', uc.mem_read(sp, 20))
            next_cargo = native_live.get(cargo + 0xB0)
            head = native_live.get(unit + 0xAC)
            event = {'tick': native_live.get(GS + 0x19F44),
                     'args': args, 'active_cargo': cargo,
                     'head_before': head,
                     'owner_before': native_live.get(cargo + 0xA8)}
            if args[0] != cargo:
                raise AssertionError(('native detach callback passenger mismatch',
                                      args, hex(cargo), hex(head)))
            if head == cargo:
                native_live.put(unit + 0xAC, next_cargo)
            else:
                previous = head
                seen = set()
                while previous and previous not in seen:
                    seen.add(previous)
                    following = native_live.get(previous + 0xB0)
                    if following == cargo:
                        native_live.put(previous + 0xB0, next_cargo)
                        break
                    previous = following
                else:
                    raise AssertionError(('released cargo is absent from native list',
                                          hex(cargo), hex(head)))
            native_live.put(cargo + 0xA8, 0)
            native_live.put(cargo + 0xB0, 0)
            event.update({'head_after': native_live.get(unit + 0xAC),
                          'owner_after': native_live.get(cargo + 0xA8)})
            native_detach_events.append(event)
            return 5, 0

        p.icd.hooks[0x51B4F0] = detach_native_passenger
        if probe_native_occupancy:
            from unicorn import UC_HOOK_CODE
            from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

            def observe_native_placement(uc, address, _size, _data):
                if (address != 0x507D10 or not native_placement_trace_active[0] or
                        not native_real_placement_enabled[0]):
                    return
                esp = uc.reg_read(UC_X86_REG_ESP)
                return_address = struct.unpack('<I', uc.mem_read(esp, 4))[0]
                args = struct.unpack('<5I', uc.mem_read(esp + 4, 20))
                first_record = unit + 0x138
                first_id = struct.unpack('<H', uc.mem_read(first_record + 2, 2))[0]
                first_flags = struct.unpack('<I',
                    uc.mem_read(first_record + 0x130, 4))[0]
                first_origin = struct.unpack('<hh',
                    uc.mem_read(first_record + 0x74, 4))
                candidate_x, candidate_z = struct.unpack(
                    '<hh', struct.pack('<I', args[2]))
                occupied_cell_ids = [struct.unpack('<H', uc.mem_read(
                    p.cells_addr + (z * width + x) * 14, 2))[0]
                    for z in range(candidate_z, candidate_z + passenger_profile[1])
                    for x in range(candidate_x, candidate_x + passenger_profile[0])]
                foot_x, foot_z = passenger_profile[:2]
                first_position = struct.unpack('<3i', uc.mem_read(first_record + 0x68, 12))
                position_origin = (
                    (first_position[0] // 65536 - (foot_x - 1) * 8) // 16,
                    (first_position[2] // 65536 - (foot_z - 1) * 8) // 16)
                first_cells = [struct.unpack('<H', uc.mem_read(
                    p.cells_addr + (z * width + x) * 14, 2))[0]
                    for z in range(first_origin[1], first_origin[1] + foot_z)
                    for x in range(first_origin[0], first_origin[0] + foot_x)]
                overlaps_first = (first_origin[0] < candidate_x + foot_x and
                                  candidate_x < first_origin[0] + foot_x and
                                  first_origin[1] < candidate_z + foot_z and
                                  candidate_z < first_origin[1] + foot_z)
                assert (first_id == 2 and first_flags & 0x1000000 and
                        first_origin == position_origin and first_cells == [2] *
                        (foot_x * foot_z)), (
                    'live Araarch is not consistently registered in retail '
                    'entity/map-cell occupancy', hex(first_record), first_id,
                    hex(first_flags), first_origin, position_origin, first_cells)
                native_occupancy_live_trace.append((
                    native_occupancy_probe_tick[0], first_origin,
                    (candidate_x, candidate_z), overlaps_first,
                    occupied_cell_ids))
                handle = []

                def observe_placement_return(machine, return_ip, _return_size, _user):
                    result = machine.reg_read(UC_X86_REG_EAX)
                    native_placement_results.append((args, result))
                    placement_blocker_states.append(active_blocker_state)
                    if not result:
                        blocked_placement_states.append(active_blocker_state)
                        if native_live:
                            blocked_cargo_held.append(
                                native_live.get(native_live.carrier + 0xAC) ==
                                active_native_passenger[0] and
                                native_live.get(active_native_passenger[0] + 0xA8) ==
                                native_live.carrier)
                    machine.hook_del(handle[0])

                handle.append(uc.hook_add(UC_HOOK_CODE, observe_placement_return,
                                          begin=return_address,
                                          end=return_address))
                native_placement_return_hooks.append((args, return_address, handle[0]))

            p.uc.hook_add(UC_HOOK_CODE, observe_native_placement,
                          begin=0x507D10, end=0x507D10)
    else:
        unit = p.unit(sx - fx // 2, sz - fz // 2)
        mover = None
        type_address = TYPE
    assert p.construct() is None
    if mover is None:
        mover = struct.unpack('<I', p.uc.mem_read(unit + 8, 4))[0]
    goal_cell = ((target_x - (fx - 1) * 8) // 16,
                 (target_z - (fz - 1) * 8) // 16)
    if native_live:
        # Start the actual surface-unload handler on the same map-backed carrier
        # that will search, move, arrive and release its passenger below.
        p.uc.mem_write(unit + 0x68, struct.pack('<iii', route_header[1],
                                               sea * 65536, route_header[2]))
        p.uc.mem_write(unit + 0x74, struct.pack('<hh', sx - fx // 2,
                                               sz - fz // 2))
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
        p.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
        p.uc.mem_write(type_address + 0x23e, struct.pack('<H', transport_dist))
        # Retail's boat scanner reads the exploration-plane dimensions from
        # the owner record and sight distance from the unit type block.
        p.uc.mem_write(native_live.owner + 0x8c, struct.pack('<II', width // 2,
                                                             height // 2))
        p.uc.mem_write(type_address + 0x226, struct.pack('<h', carrier_sight))
        p.uc.mem_write(native_live.mission + 0x22,
                       struct.pack('<i', target_x * 65536))
        p.uc.mem_write(native_live.mission + 0x26, bytes(4))
        p.uc.mem_write(native_live.mission + 0x2a,
                       struct.pack('<i', target_z * 65536))
        native_live.put(unit + 0xc4, native_live.mission + 0x12)
        native_live.put(native_live.passenger + 0xc4, native_live.mission + 0x12)
        p.uc.mem_write(0x64186c, struct.pack('<I', route_tick))
        initial = native_live.dispatch(route_tick)
        assert initial[0:3] == (1, 1, 0x701), initial
        assert native_live.requests == [1], native_live.requests
        controller = native_live.get(native_live.nav + 4)
        assert controller and native_live.get(controller + 4) == native_live.mission
        assert native_live.controller_goal() == (
            0x5f28d8, goal_cell, circle_radius), native_live.controller_goal()
        p.attach_live_request(unit, mover, native_live.nav, controller,
                              (sx - fx // 2, sz - fz // 2), (fx, fz))
    else:
        p.plant_request(unit, (sx - fx // 2, sz - fz // 2), goal_cell)
    if carrier:
        if not native_live:
            p.uc.mem_write(unit + 0x68, struct.pack('<iii', route_header[1],
                                                   sea * 65536, route_header[2]))
    else:
        p.uc.mem_write(unit + 0x68, struct.pack('<iii', start_x * 65536,
                                               sea * 65536, start_z * 65536))
    p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
    p.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
    p.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
    p.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
    p.uc.mem_write(type_address + 0x18e, struct.pack('<H', turn))
    p.uc.mem_write(type_address + 0x172, struct.pack('<i', road))
    p.uc.mem_write(type_address + 0x260, struct.pack('<I', 0x80000 if heavy else 0))
    p.uc.mem_write(type_address + 0x16e, struct.pack('<i', water))
    p.uc.mem_write(type_address + 0x192, struct.pack('<hh', max_water, min_water))
    if carrier:
        p.uc.mem_write(type_address + 0x249, bytes([half_cell_ticks]))
        p.uc.mem_write(OBJ + 0x1ad, struct.pack('<I', retry))
    p.uc.mem_write(p.GRID + 4, struct.pack('<hh', fx, fz))

    independent_grade = None
    if native_map_grades:
        native_grade_profiles = {
            'lake lokken': {'vertrans', 'aratrans', 'verscout', 'verman', 'creiron',
                            'arawar', 'crester', 'npcbotl', 'npcrixx', 'verharp'},
            'per mare per terras': {'vertrans'},
            'sea dragon spine': {'vertrans', 'arawar'},
            'cairbray coast landing': {'vertrans'},
        }
        supported_carriers = native_grade_profiles.get(map_name.lower(), set())
        if not carrier or carrier.lower() not in supported_carriers:
            raise ValueError('--native-map-grades currently checks Lake Lokken '
                             '(Vertrans/Aratrans/VerScout/VerMan/Creiron/Arawar/'
                             'Crester/NpcBotl/NpcRixx/VerHarp), '
                             'Per Mare Per Terras (Vertrans), and Sea Dragon Spine '
                             '(Vertrans/Arawar)')
        tnt_data = cat(hpitool, Path(retail_root), 'maps.hpi',
                       f'Maps/{map_name}.tnt')
        map_data = parse_tnt(tnt_data)
        feature_name_pointer = struct.unpack_from('<I', tnt_data, 24)[0]
        feature_count = map_data[5]
        map_feature_names = [
            tnt_data[feature_name_pointer + index * 132 + 4:
                     feature_name_pointer + index * 132 + 132].split(b'\0', 1)[0]
            .decode('latin1') for index in range(feature_count)]
        movement, profile, native_fx, native_fz = native_water_profile(
            hpitool, retail_root, carrier)
        if (native_fx, native_fz) != (fx, fz):
            raise AssertionError(('native map-grade footprint', movement,
                                  (native_fx, native_fz), (fx, fz)))
        (class_fx, class_fz, max_depth, min_depth, bad_max_depth,
         bad_min_depth, max_slope, bad_slope, max_water_slope,
         bad_water_slope) = struct.unpack('<6h4B', profile)
        carrier_fields = retail_unit_profile(hpitool, retail_root, carrier,
                                             crusades)
        if carrier_fields.get('movementclass', '').lower() != movement.lower():
            raise AssertionError(('native map-grade movement class differs from selected FBI',
                                  movement, carrier_fields.get('movementclass'),
                                  carrier_fields))
        carrier_sight = int(float(carrier_fields.get('sightdistance', '0')))
        p.uc.mem_write(type_address + 0x226, struct.pack('<h', carrier_sight))
        # 0x507fb0 reads all of the movement class's soft and hard limits from
        # the unit type. Setting only the hard depth pair left the bad-depth
        # thresholds zero, and the boat scanner needs the authored sight range
        # to probe as far ahead as retail.
        p.uc.mem_write(type_address + 0x192, struct.pack('<4h', max_depth,
            min_depth, bad_max_depth, bad_min_depth))
        p.uc.mem_write(type_address + 0x23c, bytes((max_slope, bad_slope,
            max_water_slope, bad_water_slope)))
        independent_grade = native_grade_reader(map_data, profile)

    query_count = 0
    native_grade_queries = set()

    def grade(_uc, args):
        nonlocal query_count
        x, z = struct.unpack('<ii', p.uc.mem_read(args, 8))
        query_count += 1
        if not (0 <= x < width and 0 <= z < height):
            value = 0
        elif independent_grade:
            value = independent_grade(x, z)
            native_grade_queries.add((x, z))
            # 0x508cd0 returns the terrain/cache score. Keep the separately
            # sourced World grade-5 visibility fallback, which is not a terrain
            # score and otherwise makes unexplored cells look traversable.
            if grades[z * width + x] == 5:
                value = 5
        else:
            value = grades[z * width + x]
        return 3, value

    p.icd.hooks[0x4139d0] = grade
    # Only asset-backed carriers export a completed World attempt. Synthetic
    # routes retain the native scheduler weight from the controlled Phase.
    if completed_attempt is not None:
        captured_weight = completed_attempt[8]

        def request_weight(_uc, args):
            address = struct.unpack('<I', p.uc.mem_read(args, 4))[0]
            p.uc.mem_write(address, struct.pack('<I', captured_weight))
            return 1, address

        # Match the request's scheduler weight from the actual World boundary. The
        # standalone fixture does not have retail's full player queue, and the
        # weight can differ for another movement/footprint class.
        p.icd.hooks[0x4161b0] = request_weight
    # Let native reconstruction deliver through retail's actual navigator
    # setter. Replacing 0x4e4ea0 with a callback would capture the route but
    # leave the mover without its installed controller/path.
    if not native_live:
        _, error = p.icd.call(0x4e2500,
            (p.HANDLE + 0x1000, target_x * 65536, target_z * 65536, circle_radius), ecx=p.HANDLE)
        assert error is None, error
    native_goal = tuple(struct.unpack('<hh', p.uc.mem_read(p.HANDLE + 8, 4)))
    native_radius, radius_squared = struct.unpack('<ii', p.uc.mem_read(p.HANDLE + 0x0c, 8))
    expected_radius_squared = int(circle_radius * circle_radius / 256 + 0.5)
    assert (native_goal, native_radius, radius_squared) == \
        (goal_cell, circle_radius, expected_radius_squared), \
        (native_goal, goal_cell, native_radius, radius_squared)
    native_initial_weights = []

    def run_native_search():
        _, error = p.init()
        assert error is None, error
        native_initial_weights.append(p.get(0x54))
        p.uc.mem_write(OBJ + 0x165, struct.pack('<I', 10_000_000))
        p.uc.mem_write(OBJ + 0x5c, struct.pack('<I', 1))
        _, error = p.step()
        assert error is None, error
        # 415b10 can deliver a direct trace route on this first handoff. Detect
        # the route in the navigator because delivery now runs through retail's
        # actual 0x4e4ea0 setter, not a replacement callback.
        completed = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0] != 0
        if not completed:
            assert p.phase() == 2, 'retail reachability tracer rejected the connected map route'
            assert struct.unpack('<I', p.uc.mem_read(OBJ + 4, 4))[0] != 0, \
                'phase-2 handoff did not initialize retail cost-search heap'
            for search_step in range(100_000):
                value, error = p.step()
                assert error is None, (search_step, error)
                if value:
                    completed = True
                    _, error = p.icd.call(0x414450, (0,), ecx=OBJ)
                    assert error is None, error
                    break
        assert completed, completed
        count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
        words = struct.unpack('<' + 'h' * (count * 2),
                              p.uc.mem_read(p.NAV + 12, count * 4)) if count else ()
        return list(zip(words[::2], words[1::2]))

    native_route = run_native_search()
    native_blocker = None
    if shore_blocker:
        # Register the same mobile body in retail's live entity table and the
        # TNT cell occupancy plane after the initial route has been delivered.
        # This keeps the fixture's arrival route controlled while native mover
        # scans and placement see the actual blocker on subsequent ticks.
        entity_table = p._alloc(16 * 0x138)
        blocker_id = 2
        blocker = entity_table + blocker_id * 0x138
        blocker_nav = p._alloc(0x180)
        blocker_type = p._alloc(0x400)
        blocker_grid = p._alloc(0x40)
        unit_text = cat(hpitool, Path(retail_root), 'data.hpi',
                        f'units/{passenger}.fbi').decode('latin1')
        unit_fields = unit_properties(unit_text)
        moveinfo_text = cat(hpitool, Path(retail_root), 'data.hpi',
                            'gamedata/moveinfo.tdf').decode('latin1')
        class_fields = next((properties(block_text) for block_text in
            re.findall(r'\[[^]]+\]\s*\{([^{}]*)\}', moveinfo_text, re.S)
            if properties(block_text).get('name', '').lower() ==
               unit_fields.get('movementclass', '').lower()), None)
        if class_fields is None:
            raise AssertionError(('missing passenger movement class', unit_fields))
        class_bytes = class_record(class_fields)
        p.uc.mem_write(GS + 0x14e84,
                       struct.pack('<II', entity_table, entity_table + 16 * 0x138))
        p.uc.mem_write(blocker_grid + 4, class_bytes)
        p.uc.mem_write(blocker_type + 0x18a, struct.pack('<I', blocker_grid))
        p.uc.mem_write(blocker_type + 0x126,
                       struct.pack('<hh', *passenger_profile[:2]))
        for key, offset, default in (('maxvelocity', 0x162, 0),
                                     ('brakerate', 0x166, 0.5),
                                     ('acceleration', 0x16a, 0.5)):
            p.uc.mem_write(blocker_type + offset,
                           struct.pack('<i', int(float(unit_fields.get(key, default)) * 65536)))
        for key, offset, default in (('turnrate', 0x18e, 500),
                                     ('turninplacerate', 0x190, 0)):
            p.uc.mem_write(blocker_type + offset,
                           struct.pack('<H', int(float(unit_fields.get(key, default))) & 65535))
        blocker_road = int(float(unit_fields.get('roadmultiplier', 1.2)) * 65536)
        blocker_water = int(float(unit_fields.get('watermultiplier', 1)) * 65536)
        blocker_maximum = int(float(unit_fields.get('maxvelocity', 0)) * 65536)
        best_speed = max(blocker_road, blocker_water, 65536) * blocker_maximum >> 16
        half_cell_ticks = 255 if best_speed <= 0 else max(1, min(255, (8 * 65536) // best_speed))
        p.uc.mem_write(blocker_type + 0x172, struct.pack('<i', blocker_road))
        p.uc.mem_write(blocker_type + 0x16e, struct.pack('<i', blocker_water))
        p.uc.mem_write(blocker_type + 0x249, bytes((half_cell_ticks,)))
        p.uc.mem_write(blocker_type + 0x192, class_bytes[4:12])
        p.uc.mem_write(blocker_type + 0x23c, bytes((class_bytes[12], class_bytes[14])))
        p.uc.mem_write(blocker_type + 0x24a, b'\x01')
        p.uc.mem_write(blocker + 2, struct.pack('<H', blocker_id))
        p.uc.mem_write(blocker + 8, struct.pack('<I', blocker_nav))
        p.uc.mem_write(blocker + 0xb4, struct.pack('<I', blocker_type))
        p.uc.mem_write(blocker + 0x130, struct.pack('<I', 0x1000000))
        p.uc.mem_write(blocker + 0x12b, struct.pack('<i', blocker_maximum))
        previous_cells = set()

        def set_native_blocker(state):
            nonlocal previous_cells
            for x, z in previous_cells:
                p.uc.mem_write(p.cells_addr + (z * width + x) * 14,
                               struct.pack('<H', 0))
            previous_cells = set()
            x_raw, z_raw, speed_raw, heading = state[:4]
            foot_x, foot_z = passenger_profile[:2]
            cell_x = (x_raw - (foot_x - 1) * 8 * 65536) // (16 * 65536)
            cell_z = (z_raw - (foot_z - 1) * 8 * 65536) // (16 * 65536)
            p.uc.mem_write(blocker + 0x68, struct.pack('<iii', x_raw, 0, z_raw))
            p.uc.mem_write(blocker + 0x74, struct.pack('<hh', cell_x, cell_z))
            p.uc.mem_write(blocker + 0x78, struct.pack('<hh', foot_x, foot_z))
            p.uc.mem_write(blocker + 0x7e, struct.pack('<H', heading))
            p.uc.mem_write(blocker_nav + 0x20, struct.pack('<i', speed_raw))
            p.uc.mem_write(blocker_nav + 0x36, struct.pack('<H', 0))
            for z in range(max(0, cell_z), min(height, cell_z + foot_z)):
                for x in range(max(0, cell_x), min(width, cell_x + foot_x)):
                    p.uc.mem_write(p.cells_addr + (z * width + x) * 14,
                                   struct.pack('<H', blocker_id))
                    previous_cells.add((x, z))

        native_blocker = set_native_blocker
    if native_live:
        # The World physical trace intentionally suspends the mission poll so
        # both movers follow the already delivered segment until circle arrival.
        # For the blocker case, wake the real dispatcher as the carrier enters
        # the authored circle so subsequent blocked-placement retry deadlines
        # are owned by retail's live mission logic.
        first_poll_delay = 2200 if shore_blocker else native_map_mover_steps + 1000
        native_live.put(native_live.mission + 0x0a,
                        route_tick + first_poll_delay)
    if terrain_scan_after is not None:
        if not native_map_mover_steps or not carrier:
            raise ValueError('--terrain-scan-after requires a carrier map mover trace')
        native_scan_deadline = route_tick + terrain_scan_after
    else:
        native_scan_deadline = route_tick + native_map_mover_steps + 1000
    if independent_grade:
        map_features = map_data[4]
        observed_features = set()
        for x, z in native_grade_queries:
            # 0x508cd0 tests the footprint plus its one-cell cached-clearance
            # border through four raw-rectangle probes.
            for qz in range(max(0, z - 1), min(height, z + fz + 1)):
                for qx in range(max(0, x - 1), min(width, x + fx + 1)):
                    feature = map_features[qz * width + qx]
                    if feature < 0xfffa:
                        observed_features.add(feature)
        observed_names = {map_feature_names[index] for index in observed_features
                          if index < len(map_feature_names)}
        assert observed_names <= {'TarWave05'}, (
            'native map-grade route probe encountered an unmodeled feature', observed_names)
    if carrier:
        world_raw_pixels = [(x * 16 + (fx % 2) * 8,
                             z * 16 + (fz % 2) * 8)
                            for x, z in world_raw]
        assert native_route == world_raw_pixels, (native_route, world_raw_pixels)
        assert world_route == world_raw_pixels[1:], (world_route, world_raw_pixels)
        assert (attempt_goal_x - fx // 2, attempt_goal_z - fz // 2) == goal_cell, \
            ((attempt_goal_x, attempt_goal_z), goal_cell)
        assert native_initial_weights and all(value == weight for value in native_initial_weights), \
            ('retail initial path weight', native_initial_weights, weight)
    else:
        assert native_route[0] == anchor, (native_route[0], anchor)
        assert native_route[1:] == world_route, (native_route[1:], world_route)

    map_mover_count = 0
    map_mover_entered_circle = False
    if native_map_mover_steps:
        if not independent_grade or not carrier:
            raise AssertionError('map mover check requires an asset-backed native-grade trace')
        (seed_x, seed_y, seed_z, seed_heading, seed_speed, seed_base, terrain_flags) = world_seed
        _max_velocity, acceleration, braking, unit_turn, waterline = world_type
        if terrain_flags != 0x1000 or seed_speed != 0 or seed_base <= 0:
            raise AssertionError(('World surface mover seed', world_seed, world_type))

        map_heights = map_data[3]
        map_features = map_data[4]
        class_fx, class_fz, max_depth, min_depth, bad_max_depth, bad_min_depth, \
            max_slope, bad_slope, max_water_slope, bad_water_slope = \
            struct.unpack('<6h4B', profile)
        if (class_fx, class_fz) != (fx, fz):
            raise AssertionError(('native mover footprint', (class_fx, class_fz), (fx, fz)))

        def terrain_grade(low, high):
            if low < sea - max_depth or high > sea - min_depth:
                return 0
            slope = high - low
            hard = max_water_slope if low < sea else max_slope
            soft = bad_water_slope if low < sea else bad_slope
            if slope > soft and slope > hard:
                return 0
            if slope > soft:
                return 4
            if low < sea - bad_max_depth or high > sea - bad_min_depth:
                return 4
            return 6

        records = bytearray(width * height * 14)
        raw_grade_words = [0] * (width * ((height + 7) // 8))
        for z in range(height):
            for x in range(width):
                index = z * width + x
                offset = index * 14
                corners = (map_heights[index],
                           map_heights[z * width + min(x + 1, width - 1)],
                           map_heights[min(z + 1, height - 1) * width + x],
                           map_heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)])
                low, high = min(corners), max(corners)
                records[offset + 4] = map_heights[index]
                records[offset + 5] = high
                records[offset + 6] = low
                struct.pack_into('<H', records, offset + 8, map_features[index])
                word = (z >> 3) * width + x
                raw_grade_words[word] |= terrain_grade(low, high) << ((z & 7) * 4)
        p.uc.mem_write(p.cells_addr, bytes(records))
        p.uc.mem_write(p.GMAP, struct.pack('<' + 'I' * len(raw_grade_words),
                                           *raw_grade_words))
        p.uc.mem_write(GS + 0x19ef8, bytes((sea,)))
        feature_count = map_data[5]
        feature_table = p._alloc(max(320, feature_count * 320))
        p.uc.mem_write(feature_table, bytes(max(320, feature_count * 320)))
        p.uc.mem_write(GS + 0x19edc, struct.pack('<I', feature_table))
        p.uc.mem_write(GS + 0x19ec0, struct.pack('<I', feature_count))

        # The native mover maintains its 128px sector index as it advances.
        sector_stride, sector_rows = (width + 7) // 8, (height + 7) // 8
        sector_records = bytearray(sector_stride * sector_rows * 10)
        for sector_z in range(sector_rows):
            for sector_x in range(sector_stride):
                block = [map_heights[z * width + x]
                         for z in range(sector_z * 8, min(height, sector_z * 8 + 8))
                         for x in range(sector_x * 8, min(width, sector_x * 8 + 8))]
                sector_records[(sector_z * sector_stride + sector_x) * 10 + 1] = max(block)
        sector_grid = p._alloc(len(sector_records))
        p.uc.mem_write(sector_grid, bytes(sector_records))
        p.uc.mem_write(GS + 0x19f18, struct.pack('<I', sector_grid))
        p.uc.mem_write(GS + 0x19f1c, struct.pack('<I', sector_stride))
        p.uc.mem_write(GS + 0x600000, struct.pack('<I', GS + 0x700000))
        visibility = struct.unpack('<I', p.uc.mem_read(GS + 0x19ef4, 4))[0]
        owner = struct.unpack('<I', p.uc.mem_read(unit + 0xb8, 4))[0]
        player = p.uc.mem_read(owner + 0xeb, 1)[0]
        p.uc.mem_write(owner + 0x8c, struct.pack('<II', width // 2,
                                                  height // 2))
        p.uc.mem_write(GS + 0x306f, bytes((player,)))
        # The World fixture starts with an unexplored map; expose the same
        # initial state to retail when the live scanner is under comparison.
        initial_visibility = 0 if terrain_scan_after is not None else 0xffff
        p.uc.mem_write(visibility, struct.pack('<' + 'H' * (width * height // 4),
            *([initial_visibility] * (width * height // 4))))
        native_explored = None
        native_cargo = None
        native_cargo_commits = 0
        native_cargo_scratch_mover = [None]
        if native_exploration:
            # Retail's sight batch (4f6a60) calls 4c6c00 for every allocated,
            # living, complete unit after the simulation batch; 4c6a70/4c6800
            # OR the owner's bit into the exploration plane. Run the original
            # producer for the carrier after every native step instead of
            # leaving the plane at its initial state. The embarked passenger's
            # producer runs too (see below).
            (sight_distance, sight_height, world_player, _world_cargo,
             cached_x, cached_z, cached_eye, cached_active) = world_sight
            from unicorn import UC_HOOK_CODE
            from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP
            explore_w, explore_h = width // 2, height // 2
            # Original coarse 32px height pairs (50ea58..50ed53), skipping the
            # sector-grid prefix this fixture authors itself.
            def coarse_entry(uc, _address, _size, _data):
                sp = uc.reg_read(UC_X86_REG_ESP) - 12
                uc.mem_write(sp, bytes(12))
                uc.reg_write(UC_X86_REG_ESP, sp)
                uc.reg_write(UC_X86_REG_EIP, 0x50ea58)
            coarse_hook = p.uc.hook_add(UC_HOOK_CODE, coarse_entry,
                                        begin=0x50e746, end=0x50e746)
            try:
                # A whole map exceeds Icd.call's default wall-clock budget, which
                # would stop emulation silently; run unbounded and require return.
                _, error = p.icd.call(0x50e740, timeout=0)
            finally:
                p.uc.hook_del(coarse_hook)
            assert error is None, ('native coarse exploration heights', error)
            assert p.uc.reg_read(UC_X86_REG_EIP) == 0x6FFFF000, 'native coarse construction did not return'
            assert struct.unpack('<2I', p.uc.mem_read(GS + 0x19f0c, 8)) == (
                explore_w, explore_h), 'native coarse exploration geometry'
            counts = p._alloc(explore_w * explore_h)
            p.uc.mem_write(counts, bytes(explore_w * explore_h))
            p.uc.mem_write(owner + 0x88, struct.pack('<I', counts))
            # 4c6a70 reads [[0x62d558]+8]+0x15/+0x16: fog removal and exploration.
            options = struct.unpack('<I', p.uc.mem_read(GS + 0x600000 + 8, 4))[0]
            if not options:
                options = p._alloc(0x100)
                p.uc.mem_write(options, bytes(0x100))
                p.uc.mem_write(GS + 0x600000 + 8, struct.pack('<I', options))
            p.uc.mem_write(options + 0x15, b'\x01\x01')
            # Initial World plane, translated from World's player bit to the
            # fixture's single native player bit.
            initial = [0] * (explore_w * explore_h)
            for (x, z), value in world_exploration_init.items():
                if value >> world_player & 1:
                    initial[z * explore_w + x] = 1 << player
            p.uc.mem_write(visibility, struct.pack('<' + 'H' * len(initial), *initial))
            sight = unit + 0x84
            p.uc.mem_write(sight, struct.pack('<I', owner))
            # Start from World's cached footprint (4c6a70 keeps it until the
            # 32px cell changes or eye height moves by more than five). The
            # fixture teleports the carrier to its seed, so this cache is part
            # of the identical initial state rather than a later result.
            p.uc.mem_write(sight + 0x14, struct.pack('<hhihBB', cached_x, cached_z,
                cached_eye, sight_distance, sight_height, cached_active))
            native_explored = {(x, z) for (x, z), value in world_exploration_init.items()
                               if value >> world_player & 1}
            world_explored = set(native_explored)
            native_exploration_reveals = 0
            # Retail's sight batch has no attachment test (4f6a60..4f6ac0): the
            # embarked passenger explores with its own sight from the attach
            # point its mover commits (4dad30 -> 4dd250 -> 51b3b0). Seed its
            # native cache from World's exactly like the carrier's.
            if native_live and world_cargo_sight is not None:
                if native_cargo_count != 1:
                    raise ValueError('--native-exploration cargo sight checks one passenger')
                native_cargo = native_live.passenger
                (cargo_distance, cargo_height, _cargo_y, cargo_x, cargo_z, cargo_eye,
                 cargo_active, cargo_waterline) = world_cargo_sight
                cargo_type = struct.unpack('<I', p.uc.mem_read(native_cargo + 0xb4, 4))[0]
                p.uc.mem_write(cargo_type + 0x248, bytes((cargo_waterline & 0xff,)))
                # Both pickup handlers attach with piece -1 (408d00/41add7).
                p.uc.mem_write(native_cargo + 0x112, b'\xff')
                p.uc.mem_write(native_cargo + 0x84, struct.pack('<I', owner))
                p.uc.mem_write(native_cargo + 0x84 + 0x14, struct.pack('<hhihBB',
                    cargo_x, cargo_z, cargo_eye, cargo_distance, cargo_height, cargo_active))
        p.uc.mem_write(GS + 0x19f44, struct.pack('<I', route_tick))
        p.uc.mem_write(0x64186c, struct.pack('<I', route_tick))

        p.uc.mem_write(unit + 0x68, struct.pack('<iii', seed_x, seed_y, seed_z))
        p.uc.mem_write(unit + 0x7e, struct.pack('<H', seed_heading))
        p.uc.mem_write(unit + 0x12b, struct.pack('<i', seed_base))
        p.uc.mem_write(mover + 0x20, struct.pack('<i', seed_speed))
        p.uc.mem_write(mover + 0x30, struct.pack('<I', native_scan_deadline))
        p.uc.mem_write(mover + 8, bytes(12))
        p.uc.mem_write(mover + 0x14, bytes(12))
        p.uc.mem_write(mover + 0x36, struct.pack('<H', flags | 1))
        p.uc.mem_write(type_address + 0x162, struct.pack('<i', seed_base))
        # Retail stores brakerate before acceleration in the native type block:
        # maxvelocity +0x162, brakerate +0x166, acceleration +0x16a.
        p.uc.mem_write(type_address + 0x166, struct.pack('<i', braking))
        p.uc.mem_write(type_address + 0x16a, struct.pack('<i', acceleration))
        p.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
        p.uc.mem_write(type_address + 0x18a, struct.pack('<I', p.GRID))
        p.uc.mem_write(type_address + 0x18e, struct.pack('<H', unit_turn))
        p.uc.mem_write(type_address + 0x192, struct.pack('<4h', max_depth,
            min_depth, bad_max_depth, bad_min_depth))
        p.uc.mem_write(type_address + 0x23c, bytes((max_slope, bad_slope,
            max_water_slope, bad_water_slope)))
        p.uc.mem_write(type_address + 0x24a, b'\x01')
        p.uc.mem_write(type_address + 0x248, bytes((waterline & 0xff,)))
        p.uc.mem_write(p.GRID + 4, struct.pack('<hh', fx, fz))
        p.uc.mem_write(p.GRID + 8, struct.pack('<4h4B', max_depth, min_depth,
            bad_max_depth, bad_min_depth, max_slope, bad_slope,
            max_water_slope, bad_water_slope))
        p.icd.hooks[0x51ad20] = lambda _uc, _args: (1, 0)
        p.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)

        # Retail game startup (0x4e6060) creates this global path-search
        # singleton before any navigator can call 0x415f30 from
        # setDestination. Phase.construct() above creates only the per-search
        # work object, so a mover that refreshes its destination otherwise
        # calls 0x415f30 with a null this pointer.
        search_service = p._alloc(0x22b)
        _, error = p.icd.call(0x415f80, (), ecx=search_service)
        assert error is None, ('native global path-search constructor', error)
        p.uc.mem_write(GS + 0x19e70, struct.pack('<I', search_service))

        live_release_step = None
        native_arrival_wakes = 0
        native_scan_count = 0
        movement_parity_steps = 0
        previous_native_scan_deadline = native_scan_deadline
        native_release_positions = {}
        native_release_ticks = {}
        native_occupancy_hold_step = None
        native_occupancy_hold_results = None
        native_occupancy_hold_trace = None
        for step, row in enumerate(world_steps, 1):
            p.uc.mem_write(GS + 0x19f44, struct.pack('<I', route_tick + step))
            if shore_blocker:
                active_blocker_state = world_blockers[step - 1]
                x_raw, z_raw, blocker_speed, heading = active_blocker_state[:4]
                native_place.set_blocker(x_raw, z_raw, blocker_speed != 0, heading,
                                         blocker_speed)
                native_blocker(active_blocker_state)
            if native_live:
                placement_result_count_before = len(native_placement_results)
                stage_before_dispatch = p.uc.mem_read(native_live.mission + 5, 1)[0]
                native_placement_trace_active[0] = probe_native_occupancy
                native_occupancy_probe_tick[0] = route_tick + step
                try:
                    dispatch_row = native_live.dispatch(route_tick + step)
                finally:
                    native_placement_trace_active[0] = False
                if stage_before_dispatch == 1 and dispatch_row[1] == 2:
                    native_arrival_wakes += 1
                for cargo in native_passengers:
                    if cargo in native_release_positions or \
                            native_live.get(cargo + 0xA8) == unit:
                        continue
                    released_cargo = struct.unpack('<3i',
                        p.uc.mem_read(cargo + 0x68, 12))
                    native_release_positions[cargo] = released_cargo
                    native_release_ticks[cargo] = step
                next_passenger = native_live.get(native_live.mission + 0x16)
                if next_passenger in native_passengers and \
                        native_live.get(next_passenger + 0xA8) == unit:
                    active_native_passenger[0] = next_passenger
                if (probe_native_occupancy and native_occupancy_probe_state is None and
                        native_passengers[0] in native_release_positions):
                    if native_cargo_count < 2 or not native_placement_results:
                        raise AssertionError(('occupancy probe lacks its next cargo or placement args',
                                              native_cargo_count, native_placement_results[-4:]))
                    first_position = native_release_positions[native_passengers[0]]
                    next_id = native_live.get(native_passengers[1] + 2)
                    native_occupancy_probe_state = native_detached_passenger_occupancy_probe(
                        p, unit, native_live.owner, passenger_profile,
                        passenger_profile_fields, moveinfo_text, first_position,
                        next_id, native_placement_results[-1][0])
                    # The callback fixture initially gives every passenger the
                    # carrier's type pointer. Retail 0x408d50 reads +0xb4 from
                    # the actual cargo record before calling 0x507d10, so give
                    # passenger 2 the authored Araarch placement type used by
                    # the same-Phase occupancy query.
                    native_live.put(native_passengers[1] + 0xB4,
                                    native_occupancy_probe_state[4])
                    if native_detach_second:
                        from unicorn import UC_HOOK_CODE
                        from unicorn.x86_const import UC_X86_REG_ESP

                        cargo2_fixture = native_passengers[1]
                        cargo2 = unit + 2 * 0x138  # global entity id 3
                        if cargo2_fixture != cargo2:
                            p.uc.mem_write(cargo2, bytes(p.uc.mem_read(
                                cargo2_fixture, 0x138)))
                            native_live.put(cargo2_fixture + 0xA8, 0)
                            native_live.put(cargo2_fixture + 0xB0, 0)
                        native_passengers[1] = cargo2
                        native_live.put(cargo2 + 2, 3)
                        native_live.put(cargo2 + 0xA8, 0)
                        native_live.put(cargo2 + 0xB0, 0)
                        native_live.put(cargo2 + 0xB4,
                                        native_occupancy_probe_state[4])
                        native_live.put(cargo2 + 0xB8, native_live.owner)
                        p2_mover, p2_nav = p._alloc(0x180), p._alloc(0x180)
                        p.uc.mem_write(p2_mover, bytes(0x180))
                        p.uc.mem_write(p2_nav, bytes(0x180))
                        p.uc.mem_write(cargo2 + 8, struct.pack('<I', p2_mover))
                        p.uc.mem_write(p2_mover, struct.pack('<I', p2_nav))
                        p.uc.mem_write(p2_nav, struct.pack('<I', 0x5f2a24))
                        p.uc.mem_write(p2_nav + 8, struct.pack('<I', cargo2))
                        first_position = native_release_positions[native_passengers[0]]
                        p.uc.mem_write(cargo2 + 0x68,
                                       struct.pack('<iii', *first_position))
                        p.uc.mem_write(cargo2 + 0x78,
                                       struct.pack('<hh', *passenger_profile[:2]))

                        # A carried entity keeps its prior sector pointer after
                        # it leaves the sector chain. Recreate that native
                        # boarding state so 0x51b5a0 can reinsert this record.
                        sector_stride = struct.unpack('<I', p.uc.mem_read(
                            GS + 0x19F1C, 4))[0]
                        sector_grid = struct.unpack('<I', p.uc.mem_read(
                            GS + 0x19F18, 4))[0]
                        sector_x = first_position[0] >> 23
                        sector_z = first_position[2] >> 23
                        sector = sector_grid + (sector_z * sector_stride +
                                                sector_x) * 10
                        _, error = p.icd.call(0x506650, (cargo2, sector))
                        assert error is None, ('native passenger-2 sector insertion', error)
                        _, error = p.icd.call(0x5065F0, (cargo2,), ecx=sector)
                        assert error is None, ('native passenger-2 board removal', error)
                        assert struct.unpack('<I', p.uc.mem_read(
                            cargo2 + 0xA4, 4))[0] == sector
                        assert struct.unpack('<I', p.uc.mem_read(
                            sector + 6, 4))[0] != cargo2
                        native_live.put(cargo2 + 0xA8, unit)
                        native_live.put(unit + 0xAC, cargo2)
                        native_live.put(native_live.mission + 0x16, cargo2)
                        active_native_passenger[0] = cargo2

                        def observe_native_detach_entry(uc, address, _size, _data):
                            if address not in (0x51B4F0, 0x51B5A0):
                                return
                            esp = uc.reg_read(UC_X86_REG_ESP)
                            if address == 0x51B4F0:
                                args = struct.unpack('<5I', uc.mem_read(esp + 4, 20))
                                native_detach_native_entries.append((
                                    native_occupancy_probe_tick[0], address, args))
                            else:
                                record = struct.unpack('<I', uc.mem_read(esp + 4, 4))[0]
                                payload = bytes(uc.mem_read(record, 7))
                                native_detach_native_entries.append((
                                    native_occupancy_probe_tick[0], address,
                                    record, payload))

                        p.uc.hook_add(UC_HOOK_CODE, observe_native_detach_entry,
                                      begin=0x51B4F0, end=0x51B5A0)
                        removed_detach_hook = p.icd.hooks.pop(0x51B4F0, None)
                        if removed_detach_hook is None:
                            raise AssertionError('diagnostic detach hook was missing')
                        print(f'  Native detach variant: passenger 2 is entity id 3 at '
                              f'{hex(cargo2)} with a valid, removed sector link at '
                              f'{hex(sector)}; retail 0x51b4f0 is unhooked.')
                    placement_hook = p.icd.hooks.pop(0x507D10, None)
                    if placement_hook is None:
                        raise AssertionError('native placement oracle hook disappeared early')
                    native_real_placement_enabled[0] = True
                    print('  GROUND_UNLOAD now uses retail 0x507d10 directly for '
                          'passenger 2; the detached passenger remains active in '
                          'the same Phase entity and cell occupancy tables.')
                    native_place.set_blocker(first_position[0], first_position[2],
                                             moving=False)
                    if native_occupancy_probe_state[0] != 0:
                        raise AssertionError(('native 0x507d10 accepted an occupied unload cell',
                                              native_occupancy_probe_state))
            else:
                value, error = p.icd.call(0x4d8450, (unit,))
                assert error is None, ('native map mover pre-step', step, error)
            if terrain_scan_after is not None:
                # The mission dispatcher temporarily changes this global
                # context pointer. Restore the same scan-enabled options used
                # by the World trace before retail's mover runs.
                p.uc.mem_write(0x62d558, struct.pack('<I', GS + 0x600000))
                p.uc.mem_write(GS + 0x600000, struct.pack('<I', GS + 0x700000))
                p.uc.mem_write(GS + 0x700000 + 0x0a, b'\0')
            # The surface mission's placement hook is a map-backed Araarch
            # release oracle. Keep retail's own carrier-footprint mover scan
            # active during 0x4dc800 rather than routing it through that hook.
            def commit_native_cargo():
                # 4dad30's attached branch for the passenger: 4dd250 attach
                # point, floater clamp, then 51b3b0 stores +0x68. Observe the
                # committed point; the cell restamp inside 51b3b0 is outside
                # this check (World keeps cargo out of the occupancy tables).
                committed = []

                def position_commit(uc, sp):
                    who, x, y, z, _flags = struct.unpack('<Iiiii', uc.mem_read(sp, 20))
                    committed.append((x, y, z))
                    uc.mem_write(who + 0x68, struct.pack('<iii', x, y, z))
                    return 5, 0
                saved_hooks = p.icd.hooks
                p.icd.hooks = dict(saved_hooks)
                p.icd.hooks[0x51b3b0] = position_commit
                try:
                    cargo_mover = struct.unpack('<I', p.uc.mem_read(native_cargo + 8, 4))[0]
                    if not cargo_mover:
                        # The release fixture gives the cargo no mover; the
                        # position segment only reads its +0x36 state bits.
                        if native_cargo_scratch_mover[0] is None:
                            native_cargo_scratch_mover[0] = p._alloc(0x180)
                            p.uc.mem_write(native_cargo_scratch_mover[0], bytes(0x180))
                        cargo_mover = native_cargo_scratch_mover[0]
                    # Stop after the position commit (4dadd7): the orientation
                    # copy that follows (4dd370) reads the host's drawable,
                    # which this fixture does not build, and is not sight input.
                    stop = p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(),
                                         begin=0x4dadd7, end=0x4dadd7)
                    try:
                        _, cargo_error = p.icd.call(0x4dad30, (native_cargo,), ecx=cargo_mover,
                                                    allow_early_stop=True)
                    finally:
                        p.uc.hook_del(stop)
                    assert p.uc.reg_read(UC_X86_REG_EIP) == 0x4dadd7, (
                        'native cargo position commit did not finish', step)
                finally:
                    p.icd.hooks = saved_hooks
                assert cargo_error is None, ('native cargo position commit', step, cargo_error)
                assert len(committed) == 1, ('native cargo commit count', step, committed)
                return committed[0]
            cargo_attached = (native_cargo is not None and
                              native_live.get(native_cargo + 0xa8) == unit and
                              step in world_cargo_steps)
            native_cargo_point = None
            if cargo_attached and world_cargo_first:
                native_cargo_point = commit_native_cargo()
            placement_hooks = p.icd.hooks
            if native_live:
                p.icd.hooks = {address: hook for address, hook in placement_hooks.items()
                               if address != 0x507d10}
            try:
                value, error = p.icd.call(0x4dc800, (unit,), ecx=mover)
            finally:
                p.icd.hooks = placement_hooks
            assert error is None, ('native map mover', step, error)
            if cargo_attached and not world_cargo_first:
                native_cargo_point = commit_native_cargo()
            if native_cargo_point is not None:
                if native_cargo_point != world_cargo_steps[step]:
                    raise AssertionError(('native/World attached cargo position', step,
                                          native_cargo_point, world_cargo_steps[step]))
                native_cargo_commits += 1
            value, error = p.icd.call(0x51b2a0, (unit,), ecx=mover)
            assert error is None, ('native map route update', step, error)
            # The released passenger's own sight is not produced natively; stop
            # comparing exploration on the release step.
            if native_exploration and not (native_live and not native_live.get(unit + 0xAC)):
                _, error = p.icd.call(0x4c6c00, (unit,))
                assert error is None, ('native sight update', step, error)
                if native_cargo_point is not None:
                    _, error = p.icd.call(0x4c6c00, (native_cargo,))
                    assert error is None, ('native cargo sight update', step, error)
                plane = struct.unpack('<' + 'H' * (explore_w * explore_h),
                    p.uc.mem_read(visibility, explore_w * explore_h * 2))
                now = {(i % explore_w, i // explore_w) for i, value in enumerate(plane)
                       if value >> player & 1}
                for x, z, value in world_exploration_deltas.get(step, ()):
                    if value >> world_player & 1:
                        world_explored.add((x, z))
                if now != world_explored:
                    raise AssertionError(('native/World exploration after step', step,
                        sorted(now - world_explored)[:16], sorted(world_explored - now)[:16]))
                native_exploration_reveals += len(now - native_explored)
                native_explored = now
            move_flags = struct.unpack('<H', p.uc.mem_read(mover + 0x36, 2))[0]
            if terrain_scan_after is not None:
                native_scan_deadline = struct.unpack('<I',
                    p.uc.mem_read(mover + 0x30, 4))[0]
                world_scan_deadline = row[10]
                assert native_scan_deadline == world_scan_deadline, (
                    'native/World local terrain-scan deadline', step,
                    native_scan_deadline, world_scan_deadline, row)
                if native_scan_deadline != previous_native_scan_deadline:
                    native_scan_count += 1
                previous_native_scan_deadline = native_scan_deadline
            native_step = (*struct.unpack('<iii', p.uc.mem_read(unit + 0x68, 12)),
                           struct.unpack('<H', p.uc.mem_read(unit + 0x7e, 2))[0],
                           struct.unpack('<i', p.uc.mem_read(mover + 0x20, 4))[0],
                           (move_flags >> 5) & 7, (move_flags >> 8) & 7,
                           move_flags & 0x1800)
            expected_step = (*row[1:6], *row[6:9])
            assert native_step == expected_step, (
                'native/World real-map mover step', step, native_step,
                expected_step, row[9])
            movement_parity_steps += 1
            new_placements = native_placement_results[placement_result_count_before:]
            same_candidate_pair = (len(new_placements) >= 2 and
                new_placements[-2][1] == 0 and new_placements[-1][1] == 1 and
                new_placements[-2][0][2] == new_placements[-1][0][2] and
                new_placements[-2][0][1] == native_live.get(native_passengers[1] + 2))
            if (probe_native_occupancy and native_occupancy_probe_state is not None and
                    same_candidate_pair and
                    native_live.get(unit + 0xac) == native_passengers[1] and
                    native_live.get(native_passengers[1] + 0xa8) == unit):
                native_occupancy_hold_step = step
                native_occupancy_hold_results = tuple(native_placement_results[-2:])
                native_occupancy_hold_trace = tuple(native_occupancy_live_trace[-2:])
                print(f'  Native GROUND_UNLOAD held passenger 2 at the original shore '
                      f'candidate on physical step {step}; native placement returned '
                      f'strict=0 then allow-moving=1, while its cargo owner and carrier '
                      f'list link remained intact.')
                break
            if native_live and not native_live.get(unit + 0xAC):
                live_release_step = step
                break
        map_mover_count = len(world_steps)
        native_park_continuation = None
        if native_live and native_occupancy_hold_step is not None:
            # Continue this exact integrated Phase. Give the now-detached first
            # Araarch retail's native code-33 order, then advance its actual
            # navigator, mover and position commit while GROUND_UNLOAD retries
            # passenger 2 through the real 0x507d10.
            first, second = native_passengers[:2]
            p1 = unit + 0x138
            p1_mover = struct.unpack('<I', p.uc.mem_read(p1 + 8, 4))[0]
            p1_nav = struct.unpack('<I', p.uc.mem_read(p1_mover, 4))[0]
            p1_kind = native_occupancy_probe_state[4]
            p1_grid = struct.unpack('<I', p.uc.mem_read(p1_kind + 0x18a, 4))[0]
            p1_foot_x, p1_foot_z = passenger_profile[:2]
            p1_fields = passenger_profile_fields
            p1_class_name = p1_fields.get('movementclass', '').lower()
            p1_class_fields = next((properties(block_text) for block_text in
                re.findall(r'\[[^]]+\]\s*\{([^{}]*)\}', moveinfo_text, re.S)
                if properties(block_text).get('name', '').lower() == p1_class_name), None)
            if p1_class_fields is None:
                raise AssertionError(('missing Araarch GROUND_PARK movement class',
                                      p1_class_name))
            p1_class_bytes = class_record(p1_class_fields)
            p.uc.mem_write(p1_grid + 4, p1_class_bytes)
            p.uc.mem_write(p1_kind + 0x126,
                           struct.pack('<hh', p1_foot_x, p1_foot_z))
            p.uc.mem_write(p1_kind + 0x260, bytes(4))  # Araarch is ground-bound.
            p.uc.mem_write(p1_kind + 0x192, p1_class_bytes[4:12])
            p.uc.mem_write(p1_kind + 0x23c,
                           bytes((p1_class_bytes[12], p1_class_bytes[14])))
            p.uc.mem_write(p1_kind + 0x24a, b'\x01')
            p1_max_velocity = int(float(p1_fields.get('maxvelocity', '0')) * 65536)
            p1_acceleration = int(float(p1_fields.get('acceleration', '0.5')) * 65536)
            p1_braking = int(float(p1_fields.get('brakerate', '0.5')) * 65536)
            p1_road = int(float(p1_fields.get('roadmultiplier', '1.2')) * 65536)
            p1_water = int(float(p1_fields.get(
                'watermultiplier', '1')) * 65536)
            p1_turn = int(float(p1_fields.get('turnrate', '500')))
            for offset, value in ((0x162, p1_max_velocity),
                                  (0x166, p1_braking),
                                  (0x16a, p1_acceleration),
                                  (0x172, p1_road), (0x16e, p1_water)):
                p.uc.mem_write(p1_kind + offset, struct.pack('<i', value))
            p.uc.mem_write(p1_kind + 0x18e, struct.pack('<H', p1_turn & 0xffff))
            p.uc.mem_write(p1 + 0x12b, struct.pack('<i', p1_max_velocity))
            p.uc.mem_write(p1_mover, struct.pack('<II', p1_nav, p.GRID))
            p.uc.mem_write(p1_mover + 0x20, struct.pack('<i', 0))
            p.uc.mem_write(p1_mover + 0x36, struct.pack('<H', 1))
            p.uc.mem_write(p.GRID + 4, struct.pack('<hh', p1_foot_x, p1_foot_z))
            p.uc.mem_write(p.GRID + 8, p1_class_bytes)

            # Install the real retail code-33 handler descriptor and order.
            definitions = struct.unpack('<I', p.uc.mem_read(0x62db84, 4))[0]
            if not definitions:
                raise AssertionError('retail mission descriptor table is missing')
            p.uc.mem_write(definitions + 33 * 25 + 4,
                           struct.pack('<I', 0x407ca0))
            p.uc.mem_write(definitions + 33 * 25 + 0x11,
                           struct.pack('<I', 0x200))
            park_order = p._alloc(0x80)
            _, error = p.icd.call(0x4d6c40,
                (33, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=park_order)
            assert error is None, ('native Araarch GROUND_PARK constructor', error)
            # Same unload-derived values as World::releasePassenger: two-cargo
            # Araarch landing gives padding 38; mobile parks are temporary.
            p.uc.mem_write(park_order + 0x4e, struct.pack('<I', 38))
            p.uc.mem_write(park_order + 0x52, struct.pack('<I', 0))
            p.uc.mem_write(park_order + 0x56, struct.pack('<I', 0))
            _, error = p.icd.call(0x4d7750, (p1, park_order))
            assert error is None, ('native Araarch GROUND_PARK insertion', error)
            assert struct.unpack('<I', p.uc.mem_read(p1 + 0x60, 4))[0] == park_order

            p1_origin = struct.unpack('<hh', p.uc.mem_read(p1 + 0x74, 4))
            p1_position = struct.unpack('<3i', p.uc.mem_read(p1 + 0x68, 12))
            tick0 = route_tick + native_occupancy_hold_step
            p.uc.mem_write(GS + 0x19f44, struct.pack('<I', tick0 + 1))
            p.uc.mem_write(0x64186c, struct.pack('<I', tick0 + 1))
            native_occupancy_probe_tick[0] = tick0 + 1
            _, error = p.icd.call(0x4d8450, (p1,))
            assert error is None, ('native Araarch GROUND_PARK dispatch', error)
            park_stage = p.uc.mem_read(park_order + 5, 1)[0]
            park_nav_controller = struct.unpack('<I', p.uc.mem_read(p1_nav + 4, 4))[0]
            park_route_count = struct.unpack('<I', p.uc.mem_read(p1_nav + 0x10c, 4))[0]
            print(f'  Native GROUND_PARK order {hex(park_order)} dispatched at tick '
                  f'{tick0 + 1}: stage={park_stage}, controller={hex(park_nav_controller)}, '
                  f'route points={park_route_count}, Araarch origin={p1_origin}, '
                  f'position={p1_position}.')

            park_route_words = struct.unpack('<4h', p.uc.mem_read(p1_nav + 12, 8))
            print(f'  Native GROUND_PARK supplied route words {park_route_words}; '
                  'advancing its installed route directly with the native mover.')

            p1_start_origin = p1_origin
            p2_release_tick = None
            p1_vacate_tick = None
            native_detach_state = None
            post_vacancy_placement_tick = None
            park_cancel_tick = None
            park_attempts = 0
            # Native GROUND_UNLOAD increments mission+0x52 on each accepted
            # placement and advances after fifteen successes. Continue only
            # through that single retail wait boundary and one following poll.
            post_vacancy_followup_ticks = 15
            post_vacancy_phase_events = []
            retry_deadline = native_live.get(native_live.mission + 0x0a)
            continuation_tick_cap = 10000
            continuation_end = continuation_tick_cap
            candidate_x, candidate_z = struct.unpack('<hh', struct.pack(
                '<I', native_placement_results[-1][0][2]))
            candidate_cells = [(x, z) for z in
                range(candidate_z, candidate_z + p1_foot_z) for x in
                range(candidate_x, candidate_x + p1_foot_x)]
            def read_candidate_cells():
                return [struct.unpack('<H', p.uc.mem_read(
                    p.cells_addr + (z * width + x) * 14, 2))[0]
                    for x, z in candidate_cells]
            from unicorn import UC_HOOK_MEM_WRITE
            from unicorn.x86_const import UC_X86_REG_EIP
            candidate_cell_writes = []
            candidate_cell_write_hooks = []
            for cell_x, cell_z in candidate_cells:
                cell_address = p.cells_addr + (cell_z * width + cell_x) * 14

                def observe_cell_write(uc, _access, address, size, value, _data,
                                       watched_cell=(cell_x, cell_z)):
                    candidate_cell_writes.append((
                        native_occupancy_probe_tick[0],
                        hex(uc.reg_read(UC_X86_REG_EIP)),
                        watched_cell, size, value))

                candidate_cell_write_hooks.append(p.uc.hook_add(
                    UC_HOOK_MEM_WRITE, observe_cell_write,
                    begin=cell_address, end=cell_address + 1))
            continuation_result_start = len(native_placement_results)
            continuation_trace_start = len(native_occupancy_live_trace)
            continuation_detach_start = len(native_detach_events)
            deadline_events = [{'tick': tick0, 'deadline': retry_deadline,
                                'cargo_head': native_live.get(unit + 0xac),
                                'second_owner': native_live.get(second + 0xa8)}]
            vacancy_events = []
            continuation_placement_events = []

            def candidate_overlaps(origin):
                return (origin[0] < candidate_x + p1_foot_x and
                        candidate_x < origin[0] + p1_foot_x and
                        origin[1] < candidate_z + p1_foot_z and
                        candidate_z < origin[1] + p1_foot_z)

            # The carrier's next live retry is scheduled for tick0+1. The
            # GROUND_PARK order was just dispatched for that tick above, so
            # process it now without dispatching the order a second time at
            # the same simulation tick. Keep both retail dispatchers and the
            # actual mover/cell commit running until the first real placement
            # call after passenger 1 vacates, a release/cancel, or tick 10000.
            for tick in range(tick0 + 1, continuation_end + 1):
                p.uc.mem_write(GS + 0x19f44, struct.pack('<I', tick))
                p.uc.mem_write(0x64186c, struct.pack('<I', tick))
                native_occupancy_probe_tick[0] = tick
                park_active = native_live.get(p1 + 0x60) == park_order
                if tick != tick0 + 1 and park_active:
                    _, error = p.icd.call(0x4d8450, (p1,))
                    assert error is None, ('native Araarch GROUND_PARK poll', tick, error)
                park_active = native_live.get(p1 + 0x60) == park_order
                current_origin = struct.unpack('<hh', p.uc.mem_read(p1 + 0x74, 4))
                current_candidate_ids = read_candidate_cells()
                if (not candidate_overlaps(current_origin) and
                        2 not in current_candidate_ids and p1_vacate_tick is None):
                    p1_vacate_tick = tick
                    vacancy_events.append({
                        'tick': tick, 'origin': current_origin,
                        'candidate_cell_ids': current_candidate_ids,
                        'deadline': native_live.get(native_live.mission + 0x0a),
                        'cargo_head': native_live.get(unit + 0xac),
                        'second_owner': native_live.get(second + 0xa8)})
                if not park_active and p1_vacate_tick is None:
                    park_cancel_tick = tick
                    break
                deadline_before = native_live.get(native_live.mission + 0x0a)
                result_before = len(native_placement_results)
                trace_before = len(native_occupancy_live_trace)
                mission_stage_before = p.uc.mem_read(
                    native_live.mission + 5, 1)[0]
                mission_events_before = native_live.get(
                    native_live.mission + 0x6a)
                native_placement_trace_active[0] = True
                try:
                    dispatch_row = native_live.dispatch(tick)
                finally:
                    native_placement_trace_active[0] = False
                mission_stage_after = (p.uc.mem_read(
                    native_live.mission + 5, 1)[0]
                    if native_live.get(unit + 0x60) == native_live.mission else None)
                mission_events_after = native_live.get(native_live.mission + 0x6a)
                deadline_after = native_live.get(native_live.mission + 0x0a)
                if deadline_after != deadline_before:
                    deadline_events.append({
                        'tick': tick, 'deadline_before': deadline_before,
                        'deadline_after': deadline_after,
                        'candidate_cell_ids': read_candidate_cells(),
                        'cargo_head': native_live.get(unit + 0xac),
                        'second_owner': native_live.get(second + 0xa8)})
                call_events = [
                    {'tick': trace[0], 'first_origin': trace[1],
                     'candidate': trace[2], 'overlaps_first': trace[3],
                     'candidate_cell_ids': trace[4], 'result': result,
                     'deadline': deadline_after,
                     'cargo_head': native_live.get(unit + 0xac),
                     'second_owner': native_live.get(second + 0xa8)}
                    for trace, (_args, result) in zip(
                        native_occupancy_live_trace[trace_before:],
                        native_placement_results[result_before:])]
                continuation_placement_events.extend(call_events)
                post_vacancy_call = any(
                    not event['overlaps_first'] and
                    2 not in event['candidate_cell_ids']
                    for event in call_events)
                if post_vacancy_call:
                    if post_vacancy_placement_tick is None:
                        post_vacancy_placement_tick = tick
                if post_vacancy_placement_tick is not None:
                    post_vacancy_phase_events.append({
                        'tick': tick, 'dispatch_row': dispatch_row,
                        'stage_before': mission_stage_before,
                        'stage_after': mission_stage_after,
                        'events_before': mission_events_before,
                        'events_after': mission_events_after,
                        'cargo_head': native_live.get(unit + 0xac),
                        'second_owner': native_live.get(second + 0xa8),
                        'placement_calls': call_events})
                if native_live.get(unit + 0xac) != second:
                    p2_release_tick = tick
                    released_position = struct.unpack('<3i',
                        p.uc.mem_read(second + 0x68, 12))
                    native_release_positions[second] = released_position
                    native_release_ticks[second] = tick
                    if native_detach_second:
                        second_sector = native_live.get(second + 0xA4)
                        native_detach_state = {
                            'entity': second,
                            'id': struct.unpack('<H', p.uc.mem_read(second + 2, 2))[0],
                            'flags': native_live.get(second + 0x130),
                            'position': released_position,
                            'owner': native_live.get(second + 0xA8),
                            'next_sector_unit': native_live.get(second + 0xB0),
                            'sector_link': second_sector,
                            'sector_head': native_live.get(second_sector + 6)
                            if second_sector else 0,
                            'carrier_cargo_head': native_live.get(unit + 0xAC),
                            'mission_stage': p.uc.mem_read(
                                native_live.mission + 5, 1)[0],
                            'candidate_cells': candidate_cells,
                            'candidate_cell_ids': read_candidate_cells(),
                        }
                    break
                if native_live.get(second + 0xa8) != unit:
                    raise AssertionError(('passenger 2 detached without list release', tick))
                if native_live.get(unit + 0xac) == second:
                    active_native_passenger[0] = second
                if (post_vacancy_placement_tick is not None and
                        tick >= post_vacancy_placement_tick +
                        post_vacancy_followup_ticks):
                    break
                if native_live.get(unit + 0x60) != native_live.mission:
                    park_cancel_tick = tick
                    break
                if not park_active:
                    # Passenger 1's PARK order completed naturally after its
                    # footprint cleared. Keep the carrier mission live until
                    # its next real placement callback or terminal condition.
                    continue
                park_attempts = struct.unpack('<I',
                    p.uc.mem_read(park_order + 0x56, 4))[0]
                _, error = p.icd.call(0x4dc800, (p1,), ecx=p1_mover)
                assert error is None, ('native Araarch GROUND_PARK mover', tick, error)
                _, error = p.icd.call(0x51b2a0, (p1,), ecx=p1_mover)
                assert error is None, ('native Araarch GROUND_PARK position commit', tick, error)
                moved_origin = struct.unpack('<hh', p.uc.mem_read(p1 + 0x74, 4))
                moved_candidate_ids = read_candidate_cells()
                if (not candidate_overlaps(moved_origin) and
                        2 not in moved_candidate_ids and p1_vacate_tick is None):
                    p1_vacate_tick = tick
                    vacancy_events.append({
                        'tick': tick, 'origin': moved_origin,
                        'candidate_cell_ids': moved_candidate_ids,
                        'deadline': native_live.get(native_live.mission + 0x0a),
                        'cargo_head': native_live.get(unit + 0xac),
                        'second_owner': native_live.get(second + 0xa8)})
            for hook in candidate_cell_write_hooks:
                p.uc.hook_del(hook)
            final_origin = struct.unpack('<hh', p.uc.mem_read(p1 + 0x74, 4))
            final_position = struct.unpack('<3i', p.uc.mem_read(p1 + 0x68, 12))
            final_overlap = (final_origin[0] < p1_start_origin[0] + p1_foot_x and
                             p1_start_origin[0] < final_origin[0] + p1_foot_x and
                             final_origin[1] < p1_start_origin[1] + p1_foot_z and
                             p1_start_origin[1] < final_origin[1] + p1_foot_z)
            continuation_placements = [
                (*trace, result)
                for trace, (_args, result) in zip(
                    native_occupancy_live_trace[continuation_trace_start:],
                    native_placement_results[continuation_result_start:])]
            native_park_continuation = {
                'first_origin': p1_start_origin, 'final_origin': final_origin,
                'final_position': final_position, 'vacate_tick': p1_vacate_tick,
                'second_release_tick': p2_release_tick, 'end_tick': tick,
                'retry_deadline': retry_deadline,
                'final_deadline': native_live.get(native_live.mission + 0x0a),
                'tick_cap': continuation_tick_cap,
                'post_vacancy_placement_tick': post_vacancy_placement_tick,
                'post_vacancy_followup_ticks': post_vacancy_followup_ticks,
                'post_vacancy_phase_events': post_vacancy_phase_events,
                'detach_events': native_detach_events[continuation_detach_start:],
                'native_detach_entries': native_detach_native_entries,
                'native_detach_state': native_detach_state,
                'park_cancel_tick': park_cancel_tick,
                'candidate_cells': candidate_cells,
                'candidate_cell_ids': read_candidate_cells(),
                'placements': continuation_placements,
                'placement_events': continuation_placement_events,
                'deadline_events': deadline_events,
                'vacancy_events': vacancy_events,
                'candidate_cell_writes': candidate_cell_writes,
                'remaining_cargo': native_live.get(unit + 0xac),
                'second_owner': native_live.get(second + 0xa8),
                'park_stage': p.uc.mem_read(park_order + 5, 1)[0],
                'park_attempts': park_attempts,
                'controller': park_nav_controller,
                'route_count': struct.unpack('<I', p.uc.mem_read(p1_nav + 0x10c, 4))[0],
                'final_overlap': final_overlap,
            }
            print(f'  Native GROUND_PARK continuation through tick {tick}: Araarch '
                  f'origin {p1_start_origin} -> {final_origin}, first vacated at '
                  f'{p1_vacate_tick} (candidate cells {candidate_cells} now have '
                  f'ids {read_candidate_cells()}); scheduled retry was '
                  f'{retry_deadline}, final deadline='
                  f'{native_park_continuation["final_deadline"]}, post-vacancy '
                  f'placement tick={post_vacancy_placement_tick}, passenger 2 '
                  f'release tick={p2_release_tick}, park cancel tick={park_cancel_tick}, '
                  f'cargo head={hex(native_live.get(unit + 0xac))}, '
                  f'park stage={native_park_continuation["park_stage"]}, '
                  f'attempts={park_attempts}, route points='
                  f'{native_park_continuation["route_count"]}; new live placement '
                  f'observations={continuation_placement_events}; deadline events='
                  f'{deadline_events}; vacancy events={vacancy_events}; '
                  f'candidate-cell writes={candidate_cell_writes}; next '
                  f'GROUND_UNLOAD phase events={post_vacancy_phase_events}; '
                  f'native detach hook events='
                  f'{native_detach_events[continuation_detach_start:]}; native '
                  f'detach body entries={native_detach_native_entries}; native '
                  f'detach state={native_detach_state}.')
        if terrain_scan_after is not None:
            assert native_scan_count > 0, ('native live terrain scan did not run',
                                           native_scan_count)
        if native_live and native_occupancy_hold_step is not None:
            first, second = native_passengers[:2]
            assert native_occupancy_hold_results is not None
            assert len(native_release_positions) in (1, 2), (
                'occupancy probe released more passengers than the two-cargo fixture',
                native_release_positions, native_release_ticks)
            assert first in native_release_positions
            if p2_release_tick is None:
                assert second not in native_release_positions
                assert native_live.get(unit + 0xac) == second
                assert native_live.get(second + 0xa8) == unit
                assert native_live.get(native_live.mission + 0x16) == second
            else:
                assert second in native_release_positions
                assert native_release_ticks[second] == p2_release_tick
                assert native_live.get(unit + 0xac) == 0
                assert native_live.get(second + 0xa8) == 0
            if native_detach_second:
                assert p2_release_tick == 3565, p2_release_tick
                assert native_detach_state is not None
                assert native_detach_state['id'] == 3
                assert native_detach_state['flags'] & 0x01000000
                assert native_detach_state['owner'] == 0
                assert native_detach_state['carrier_cargo_head'] == 0
                assert native_detach_state['sector_link'] != 0
                assert native_detach_state['sector_head'] == native_detach_state['entity']
                assert [entry[1] for entry in native_detach_native_entries] == [
                    0x51B4F0, 0x51B5A0], native_detach_native_entries
                print(f'  Native detach body state after tick {p2_release_tick}: '
                      f'{native_detach_state}.')
            assert [result for _args, result in native_occupancy_hold_results] == [0, 1]
            assert blocked_cargo_held and any(blocked_cargo_held)
            second_args = native_occupancy_hold_results[-1][0]
            assert all(args[0] == native_occupancy_probe_state[4] and
                       args[1] == native_live.get(second + 2)
                       for args, _result in native_occupancy_hold_results), (
                'live retail placement did not use passenger 2 native type/id',
                native_occupancy_hold_results)
            assert second_args[2] == native_occupancy_hold_results[-2][0][2], (
                'blocked second passenger was assigned a different placement cell',
                native_occupancy_hold_results,)
            assert native_occupancy_hold_trace is not None
            assert all(trace[2] == (second_args[2] & 0xffff,
                                    (second_args[2] >> 16) & 0xffff) and
                       trace[3] and trace[4] == [2] * 4
                       for trace in native_occupancy_hold_trace), (
                'native placement did not observe all four occupied passenger-1 cells',
                native_occupancy_hold_trace)
            if native_park_continuation:
                assert (native_park_continuation['post_vacancy_placement_tick'] is None or
                        native_park_continuation['vacate_tick'] is not None), (
                    'post-vacancy native retry was observed before cell vacancy',
                    native_park_continuation)
            print(f'  Occupancy outcome: live native 0x507d10 received Araarch type '
                  f'{hex(second_args[0])}, passenger id {second_args[1]}, and the '
                  f'{second_args[2] & 0xffff},{(second_args[2] >> 16) & 0xffff} '
                  f'candidate already occupied by passenger 1; strict=0 and '
                  f'allow-moving=1. Continuation release tick={p2_release_tick}; '
                  f'carrier cargo head={hex(native_live.get(unit + 0xac))}, '
                  f'passenger-2 owner={hex(native_live.get(second + 0xa8))}.')
            map_mover_count = native_occupancy_hold_step
        elif native_live:
            assert live_release_step is not None, (
                'native surface mission did not release Araarch during the map-backed mover trace',
                {'placement': native_placement_results[-8:],
                 'stage': p.uc.mem_read(native_live.mission + 5, 1)[0],
                 'mission_events': native_live.get(native_live.mission + 0x6A),
                 'unit_events': native_live.get(unit + 0x60),
                 'position': struct.unpack('<iii', p.uc.mem_read(unit + 0x68, 12)),
                 'navigator_controller': native_live.get(native_live.nav + 4),
                 'navigator_path_count': struct.unpack('<I', p.uc.mem_read(
                     native_live.nav + 0x10C, 4))[0],
                 'arrival_wakes': native_arrival_wakes,
                 'placement_blocker_states': placement_blocker_states,
                 'final_mission_deadline': native_live.get(native_live.mission + 0x0A),
                 'final_tick': route_tick + len(world_steps)})
            native_live.dispatch(route_tick + live_release_step + 1)
            assert native_live.get(unit + 0x60) == 0, (
                'native surface mission did not retire after its one-tick release tail',
                hex(native_live.get(unit + 0x60)),
                native_live.get(native_live.mission + 4),
                native_live.get(native_live.mission + 5),
                native_live.get(native_live.mission + 0x16),
                hex(native_live.get(unit + 0xAC)))
            assert len(native_release_positions) == native_cargo_count, (
                'native GROUND_UNLOAD did not release every linked passenger',
                len(native_release_positions), native_cargo_count,
                native_release_ticks, native_live.get(unit + 0xAC),
                native_live.get(native_live.mission + 0x16),
                native_placement_results[-8:])
            released_positions = [native_release_positions[cargo]
                                  for cargo in native_passengers]
            released_origins = [
                (int((position[0] / 65536 -
                      (passenger_profile[0] - 1) * 8) // 16),
                 int((position[2] / 65536 -
                      (passenger_profile[1] - 1) * 8) // 16))
                for position in released_positions]
            assert released_origins[0] == target_cell, (
                'retail unload did not release first Araarch at the selected map shore cell',
                released_positions[0], released_origins[0], target_cell)
            assert native_placement_results, native_placement_results
            assert all(native_live.get(cargo + 0xA8) == 0
                       for cargo in native_passengers)
            assert native_live.get(unit + 0xAC) == 0
            assert all(native_release_ticks[native_passengers[index]] <
                       native_release_ticks[native_passengers[index + 1]]
                       for index in range(native_cargo_count - 1)), (
                'native cargo list was not released in order', native_release_ticks)
            if native_cargo_count > 1:
                print(f'  Retail cargo list auto-advanced through '
                      f'{native_cargo_count} passengers on successive ticks '
                      f'{[native_release_ticks[cargo] for cargo in native_passengers]}. '
                      'This fixture does not maintain released cargo in a live retail '
                      'entity-occupancy list, so it cannot establish spatial separation '
                      'between the landing footprints.')
            if not shore_blocker:
                assert all(result == 1 for _, result in native_placement_results), \
                    native_placement_results
            if shore_blocker:
                assert native_arrival_wakes >= 1, \
                    ('native navigator arrival never woke the blocked unload mission',
                     native_arrival_wakes)
            elif native_cargo_count > 1:
                assert native_arrival_wakes >= 1, (
                    'native navigator did not wake unload for the linked cargo manifest',
                    native_arrival_wakes)
            else:
                assert native_arrival_wakes == 1, \
                    ('native navigator arrival did not wake the unload mission exactly once',
                     native_arrival_wakes)
            if shore_blocker:
                foot_x, foot_z = passenger_profile[:2]
                target_origin = (
                    (target_x - (foot_x - 1) * 8) // 16,
                    (target_z - (foot_z - 1) * 8) // 16)

                def overlaps_target(state):
                    if state is None:
                        return False
                    x_raw, z_raw, *_ = state
                    origin = (
                        (x_raw - (foot_x - 1) * 8 * 65536) // (16 * 65536),
                        (z_raw - (foot_z - 1) * 8 * 65536) // (16 * 65536))
                    return (origin[0] < target_origin[0] + foot_x and
                            target_origin[0] < origin[0] + foot_x and
                            origin[1] < target_origin[1] + foot_z and
                            target_origin[1] < origin[1] + foot_z)

                blocked_placements = [state for (_, result), state in
                                      zip(native_placement_results,
                                          placement_blocker_states) if not result]
                assert blocked_placements and any(
                    not result and args[4] == 0 and overlaps_target(state)
                    for (args, result), state in zip(native_placement_results,
                                                     placement_blocker_states)), (
                    'native map placement did not reject the live blocker on the shore',
                    native_placement_results, placement_blocker_states[:20])
                successful_placements = [state for (_, result), state in
                                         zip(native_placement_results,
                                             placement_blocker_states) if result]
                assert any(successful_placements), \
                    ('native map placement never accepted the shore after blocker clearance',
                     native_placement_results)
                clear_site_success = any(
                    result and not overlaps_target(state)
                    for (_, result), state in zip(native_placement_results,
                                                  placement_blocker_states))
                assert clear_site_success, (
                    'native placement accepted the selected site only while the blocker overlapped it',
                    native_placement_results, placement_blocker_states)
                assert any(args[4] == 1 and result and overlaps_target(state)
                           for (args, result), state in zip(native_placement_results,
                                                            placement_blocker_states)), (
                    'native relaxed placement did not recognize the live mobile blocker',
                    native_placement_results, placement_blocker_states)
                assert blocked_cargo_held and all(blocked_cargo_held), \
                    ('native dropped cargo during a blocked shoreline placement',
                     blocked_cargo_held)
                world_failed = [tick for tick, state in world_blockers.items()
                                if state[4] == 3 and state[5] > 0]
                world_released = next((tick for tick, state in world_blockers.items()
                                       if state[5] == 0), None)
                assert world_failed and world_released is not None, (
                    'World did not hold and release cargo through its live shore retry',
                    world_failed[:8], world_released)
                assert world_blockers[world_released][6] == 1, \
                    ('World blocker did not receive its clear-site move order',
                     world_blockers[world_released])
                assert not overlaps_target(world_blockers[world_released]), \
                    ('World released cargo while the blocker still overlapped the site',
                     world_blockers[world_released], target_origin)
                release_state = world_blockers[live_release_step - 1]
                assert not overlaps_target(release_state), (
                    'retail released cargo while the paired World blocker occupied the shore',
                    live_release_step, release_state, target_origin)
                assert abs(world_released - live_release_step) <= 1, (
                    'native and World live-blocker release differ by more than the '
                    'dispatcher/mover phase boundary', world_released, live_release_step)
                released_position = struct.unpack('<3i',
                    p.uc.mem_read(unit + 0x68, 12))
                carrier_x = released_position[0] / 65536
                carrier_z = released_position[2] / 65536
                assert ((carrier_x - target_x) ** 2 + (carrier_z - target_z) ** 2 <=
                        circle_radius ** 2), (
                    'retail carrier left the authored water-side unload circle during retry',
                    released_position, (target_x, target_z), circle_radius)
            map_mover_count = live_release_step
            print(f'  Native mission, search, and map mover remained joined through the '
                  f'retail shoreline release at physical step {live_release_step}; '
                  f'{native_arrival_wakes} navigator arrival wake(s); '
                  f'{len(native_placement_results)} native map-backed passenger placement '
                  f'checks ran.' +
                  (f' {native_scan_count} live terrain-scan deadlines matched.'
                   if terrain_scan_after is not None else '') +
                  (f' Retail 0x4c6c00 sight production matched World exploration through '
                   f'release, {native_exploration_reveals} newly revealed cells'
                   f'{"" if not native_cargo_commits else f", including the embarked passenger at {native_cargo_commits} native 0x4dad30 attach-point commits"}.'
                   if native_exploration else ''))
        final_row = world_steps[map_mover_count - 1]
        final_x, final_z = final_row[1], final_row[3]
        dx = final_x - target_x * 65536
        dz = final_z - target_z * 65536
        map_mover_entered_circle = dx * dx + dz * dz <= (circle_radius * 65536) ** 2
    else:
        map_mover_count = 0

    profile_name = f' {carrier}/{passenger}' if carrier else ''
    arrival = (f', endpoint inside {circle_radius}px unload circle'
               if transport_profile and world_route else '')
    grade_source = (f' native 0x508cd0 map grades on {len(native_grade_queries)} query cells '
                    f'(World grade-5 visibility retained);' if independent_grade else '')
    print(f'PASS: {map_name}{profile_name} unload route matches exactly at World tick '
          f'{route_tick}; {len(world_route)} waypoints{arrival},{grade_source} '
          f'{query_count} native grade queries, {completions} World completion(s), '
          f'{failures} World failure(s)')
    if map_mover_count:
        arrival_note = ('; both finish inside the authored unload circle'
                        if map_mover_entered_circle else
                        '; final position remains outside the unload circle')
        retry_note = ', including the blocked retry' if shore_blocker else ''
        print(f'  Native 0x4dc800 + 0x51b2a0 matches {movement_parity_steps} World physical '
              f'mover steps over TNT terrain{retry_note}{arrival_note}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('world_binary', nargs='?', default='build-o2/transport_test')
    parser.add_argument('--retail-root', default='/home/pocket_geek/tak_data')
    parser.add_argument('--map', default='Per Mare Per Terras')
    parser.add_argument('--start', nargs=2, type=int, default=(40, 120), metavar=('X', 'Z'))
    parser.add_argument('--target', nargs=2, type=int, default=(40, 142), metavar=('X', 'Z'))
    parser.add_argument('--footprint', type=int, default=4)
    parser.add_argument('--carrier', help='use an asset-backed carrier profile instead of the synthetic boat')
    parser.add_argument('--passenger', help='land passenger profile required with --carrier')
    parser.add_argument('--crusades', action='store_true', help='load Crusades unit balance')
    parser.add_argument('--native-map-grades', action='store_true',
                        help='answer retail route grades with TNT-backed native 0x508cd0')
    parser.add_argument('--native-map-mover-steps', type=int, default=0,
                        help='also compare this many native physical mover ticks on the map (1..10000)')
    parser.add_argument('--native-live-unload', action='store_true',
                        help='keep retail GROUND_UNLOAD active through map-backed movement and passenger release')
    parser.add_argument('--native-cargo-count', type=int, default=1,
                        help='number of linked cargo passengers in the native surface-unload trace (1..16)')
    parser.add_argument('--probe-native-occupancy', action='store_true',
                        help='register the first detached passenger in retail entity/cell tables and test the second landing')
    parser.add_argument('--native-detach-second', action='store_true',
                        help='unhook 0x51b4f0 for passenger 2 after preparing its native entity/sector records')
    parser.add_argument('--terrain-scan-after', type=int,
                        help='compare live native/World terrain scans after route delivery; requires --native-live-unload (0..2500 ticks)')
    parser.add_argument('--cargo-sight', type=int,
                        help='override the passenger sight distance on the World side (the '
                             'native cache is seeded from World) so embarked cargo reveals '
                             'cells its carrier cannot; requires --native-exploration')
    parser.add_argument('--native-exploration', action='store_true',
                        help='run retail sight production (0x4c6c00) for the carrier after each '
                             'native step and compare the exploration plane with World; '
                             'requires --terrain-scan-after')
    parser.add_argument('--shore-blocker', action='store_true',
                        help='move one live Araarch away after it blocks map-backed unload placement')
    parser.add_argument('--live-route-blocker-steps', type=int, default=0,
                        help='run a World path-service retry around a stationary boat on Lake Lokken (1..10000 physical ticks)')
    parser.add_argument('--native-worker-repath', action='store_true',
                        help='also replay the captured blocker route through retail 0x416430 on one retained navigator/controller; requires --live-route-blocker-steps')
    parser.add_argument('--native-worker-mission-repath', action='store_true',
                        help='also replay the blocker route through retail 0x416430 with a live GROUND_UNLOAD mission and attached passenger')
    parser.add_argument('--native-worker-mission-retry', action='store_true',
                        help='seed the captured repeated-block state into native 0x4e5150 and deliver its live mission retry with 0x416430')
    parser.add_argument('--native-worker-mission-collision', action='store_true',
                        help='follow retail from WORLDSEED to the map-backed Vertrans collision, then replay its live mission retry')
    parser.add_argument('--always-on-route-search', action='store_true',
                        help='keep World path service enabled throughout the moving blocker trace')
    parser.add_argument('--hpitool', default='build/hpitool')
    args = parser.parse_args()
    if bool(args.carrier) != bool(args.passenger):
        parser.error('--carrier and --passenger must be supplied together')
    if not 0 <= args.native_map_mover_steps <= 10000:
        parser.error('--native-map-mover-steps must be 0..10000')
    if args.native_live_unload and args.native_map_mover_steps == 0:
        parser.error('--native-live-unload requires --native-map-mover-steps')
    if not 1 <= args.native_cargo_count <= 16:
        parser.error('--native-cargo-count must be 1..16')
    if args.native_cargo_count > 1 and not args.native_live_unload:
        parser.error('--native-cargo-count above one requires --native-live-unload')
    if args.probe_native_occupancy and (not args.native_live_unload or
                                        args.native_cargo_count < 2):
        parser.error('--probe-native-occupancy requires --native-live-unload --native-cargo-count 2 or more')
    if args.native_detach_second and not args.probe_native_occupancy:
        parser.error('--native-detach-second requires --probe-native-occupancy')
    if args.terrain_scan_after is not None and not 0 <= args.terrain_scan_after <= 2500:
        parser.error('--terrain-scan-after must be 0..2500')
    if args.terrain_scan_after is not None and not args.native_live_unload:
        parser.error('--terrain-scan-after requires --native-live-unload')
    if not 0 <= args.live_route_blocker_steps <= 10000:
        parser.error('--live-route-blocker-steps must be 0..10000')
    if args.native_worker_repath and not args.live_route_blocker_steps:
        parser.error('--native-worker-repath requires --live-route-blocker-steps')
    if args.native_worker_mission_repath and not args.live_route_blocker_steps:
        parser.error('--native-worker-mission-repath requires --live-route-blocker-steps')
    if args.native_worker_mission_retry and not args.live_route_blocker_steps:
        parser.error('--native-worker-mission-retry requires --live-route-blocker-steps')
    if args.native_worker_mission_collision and not args.native_worker_mission_retry:
        parser.error('--native-worker-mission-collision requires --native-worker-mission-retry')
    if args.native_worker_mission_retry and args.always_on_route_search:
        parser.error('--native-worker-mission-retry requires the blocked, non-always-on fixture')
    if args.always_on_route_search and not args.live_route_blocker_steps:
        parser.error('--always-on-route-search requires --live-route-blocker-steps')
    check_route(args.world_binary, args.retail_root, args.map, tuple(args.start),
                tuple(args.target), args.footprint, args.carrier, args.passenger,
                args.crusades, args.native_map_grades or bool(args.native_map_mover_steps),
                str(Path(args.hpitool).resolve()), args.native_map_mover_steps,
                args.native_live_unload,args.terrain_scan_after,args.native_cargo_count,
                args.shore_blocker,
                args.live_route_blocker_steps, args.native_worker_repath,
                args.native_worker_mission_repath, args.always_on_route_search,
                args.native_worker_mission_retry,
                args.native_worker_mission_collision,
                args.probe_native_occupancy,
                args.native_detach_second,
                native_exploration=args.native_exploration,
                cargo_sight=args.cargo_sight)


if __name__ == '__main__':
    main()
