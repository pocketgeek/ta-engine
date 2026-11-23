#!/usr/bin/env python3
"""Compare retail's installed sea-unload route and mover segment transition with World.

World exports the grade plane from the actual delayed unload-circle request.
Unicorn runs the retail circle constructor, tracer, cost search, reconstruction,
and real navigator setter on those same scores, then advances 0x4dc800/0x51b2a0
from a shared post-delivery state through retry, unload, and initial coast. The
World grade plane is an explicit input.
The canonical comparison holds terrain-scan deadline and modes equal on both
sides. It follows the navigator through unload transfer and circle arrival.
"""
import argparse
import os
import struct
import subprocess

from emuphase import Phase, OBJ, TYPE, GS, ARENA, CELLS, UNITS

FEATURES = ARENA + 0x0C10000


def transform_cell(variant, x, z):
    edge = 127 if variant == 4 else 63
    if variant == 1:
        return edge - x, z
    if variant == 2:
        return x, edge - z
    if variant == 3:
        return edge - z, x
    return x, z


def transform_pixel(variant, x, z):
    edge = (127 if variant == 4 else 63) * 16
    if variant == 1:
        return edge - x, z
    if variant == 2:
        return x, edge - z
    if variant == 3:
        return edge - z, x
    return x, z


def raw_water_grid(variant, width, height):
    """Build the mover's raw terrain grades; route-search grades include footprint fitting."""
    water = bytearray(width * height)
    if variant == 4:
        rectangles = ((4, 4, 101, 13), (96, 8, 111, 112))
    elif variant == 7:
        rectangles = ((4, 4, 21, 21), (40, 40, 57, 57))
    else:
        rectangles = ((4, 4, 37, 13), (32, 8, 47, 47))
    for x0, z0, x1, z1 in rectangles:
        for z in range(z0, z1):
            for x in range(x0, x1):
                tx, tz = transform_cell(variant, x, z)
                water[tz * width + tx] = 6
    unload_cell = 104 if variant == 4 else 31 if variant == 8 else 40
    patch_size = 2 if variant == 8 else 1
    for z in range(unload_cell, unload_cell + patch_size):
        for x in range(unload_cell, unload_cell + patch_size):
            tx, tz = transform_cell(variant, x, z)
            water[tz * width + tx] = 0
    return water


def raw_surface_heights(variant, width, height):
    """Build the native cell-height plane used by the physical mover."""
    heights = [100] * (width * height)
    if variant == 4:
        rectangles = ((4, 4, 101, 13), (96, 8, 111, 112))
    elif variant == 7:
        rectangles = ((4, 4, 21, 21), (40, 40, 57, 57))
    else:
        rectangles = ((4, 4, 37, 13), (32, 8, 47, 47))
    for x0, z0, x1, z1 in rectangles:
        for z in range(z0, z1):
            for x in range(x0, x1):
                tx, tz = transform_cell(variant, x, z)
                heights[tz * width + tx] = 10
    unload_cell = 104 if variant == 4 else 31 if variant == 8 else 40
    for z in range(unload_cell, unload_cell + 2):
        for x in range(unload_cell, unload_cell + 2):
            tx, tz = transform_cell(variant, x, z)
            heights[tz * width + tx] = 100
    return heights


def check_variant(world_binary, variant, physical_steps, integrated_retry=False):
    if integrated_retry and variant != 8:
        raise ValueError('the integrated same-trip retry trace is the variant-8 water lane')
    physical_variant = variant in (0, 8)
    env = os.environ.copy()
    env['TAK_DUMP_GRADE_PLANE'] = '1'
    if physical_variant:
        env['TAK_SURFACE_STEP'] = '1'
        env['TAK_SURFACE_STEPS'] = str(physical_steps)
    world = subprocess.run([world_binary, '--surface-unload-route-fixture', str(variant)],
                           check=True, capture_output=True, text=True, env=env)
    stderr = world.stderr.splitlines()
    profile = next(line for line in stderr if line.startswith('COSTPROFILE ')).split()
    turn, fx, fz, road, water, flags, heavy, heading = map(int, profile[1:])
    header_index = next(i for i, line in enumerate(stderr) if line.startswith('GRADEPLANE '))
    width, height, grade_fx, grade_fz, retry, sx, sz, tick, grade_heading = map(
        int, stderr[header_index].split()[1:])
    start_cell = (25, 10) if variant == 8 else (10, 10)
    expected_start = transform_cell(variant, *start_cell)
    expected_foot = 3 if variant == 5 else 2 if variant == 6 else 4
    expected_size = 128 if variant == 4 else 64
    assert (width, height, grade_fx, grade_fz, retry, sx, sz) == (
        expected_size, expected_size, expected_foot, expected_foot, 0, *expected_start)
    assert (fx, fz, heavy, grade_heading) == (grade_fx, grade_fz, 1, heading)
    grades = [int(value) for line in stderr[header_index + 1:header_index + 1 + height]
              for value in line.split()]
    assert len(grades) == width * height
    assert not any(line.startswith('GRADECHANGE ') for line in stderr[header_index + 1 + height:]), \
        'the effective grade plane changed during this search; this static comparison no longer applies'

    stdout = world.stdout.splitlines()
    route_index = next(i for i, line in enumerate(stdout) if line.startswith('ROUTE '))
    route_header = list(map(int, stdout[route_index].split()[1:]))
    _, _, _, anchor_x, anchor_z, route_count, world_completions, world_failures = route_header
    if variant == 7:
        assert world_failures > 0 and world_completions == 0, route_header
    anchor = (anchor_x // 65536, anchor_z // 65536)
    world_route_fixed = [tuple(map(int, line.split()))
                         for line in stdout[route_index + 1:route_index + 1 + route_count]]
    world_route = [(x // 65536, z // 65536) for x, z in world_route_fixed]
    world_seed = tuple(map(int, next(line for line in stdout if line.startswith('WORLDSEED ')).split()[1:])) if physical_variant else ()
    world_steps = [tuple(map(int, line.split()[1:])) for line in stdout if line.startswith('WORLDSTEP ')] if physical_variant else []
    world_nav = {int(fields[0]): tuple(map(int, fields[1:]))
                 for fields in (line.split()[1:] for line in stdout if line.startswith('WORLDNAV '))} if physical_variant else {}
    world_points = {}
    if physical_variant:
        for fields in (line.split()[1:] for line in stdout if line.startswith('WORLDPOINT ')):
            step, index, x, z = map(int, fields)
            world_points.setdefault(step, []).append((x, z))

    p = Phase(width, height)
    native = None
    if integrated_retry:
        from probe_transport_surface_unload_callbacks import SurfaceUnload
        native = SurfaceUnload(
            placement_result=lambda args, _call: int(args[4] == 1),
            real_mission_removal=True, icd=p.icd, game=GS, freeze_hooks=False)
        unit = native.carrier
        mover = native.mover
        type_address = native.kind
        assert p.construct() is None
    else:
        unit = p.unit(sx - fx // 2, sz - fz // 2)
        assert p.construct() is None
    requested_target = 1664 if variant == 4 else 500 if variant == 8 else 640
    start_pixel = (400, 160) if variant == 8 else (160, 160)
    start_x, start_z = transform_pixel(variant, *start_pixel)
    target_x, target_z = transform_pixel(variant, requested_target, requested_target)
    # World::requestPath passes footprintOrigin(target, size) to the circular
    # goal. This floor is asymmetric under a pixel-space mirror at cell edges.
    expected_goal = ((target_x - (fx - 1) * 8) // 16,
                     (target_z - (fz - 1) * 8) // 16)
    if integrated_retry:
        mission_identity = native.mission
        carrier_identity = native.carrier
        passenger_identity = native.passenger
        mover_identity = native.mover
        nav_identity = native.nav
        original_destination = bytes(p.uc.mem_read(mission_identity + 0x22, 12))
        assert original_destination == struct.pack('<3i', 500 << 16, 0, 500 << 16)
        # Keep the real GROUND_UNLOAD mission alive while the native arrival
        # callback raises the moving-blocker retry. Its retained references are
        # also needed if the real mission remover retires it after disembark.
        p.uc.mem_write(native.kind + 0x126, struct.pack('<hh', fx, fz))
        p.uc.mem_write(native.kind + 0x23E, struct.pack('<H', 150))
        p.uc.mem_write(native.carrier + 0x78, struct.pack('<hh', fx, fz))
        native.put(native.carrier + 0xC4, native.mission + 0x12)
        native.put(native.passenger + 0xC4, native.mission + 0x12)
        first = native.dispatch(1)
        assert first[0:3] == (1, 1, 0x701), first
        initial_controller = native.get(native.nav + 4)
        assert initial_controller and native.get(initial_controller + 4) == mission_identity
        events, detached, arrived = native.arrive_inside_goal()
        assert events & 0x100 and detached and arrived[0:3] == (1, 2, 1), arrived
        blocked = native.dispatch(3)
        assert blocked[0:3] == (1, 3, 1), blocked
        assert native.get(native.carrier + 0x60) == mission_identity
        assert native.get(native.carrier + 0xAC) == passenger_identity
        assert native.get(native.passenger + 0xA8) == carrier_identity
        assert bytes(p.uc.mem_read(mission_identity + 0x22, 12)) == original_destination

        # The original same-trip mission is now at stage 3. Move that same live
        # carrier to the connected remote water lane, then let its own ten-tick
        # retry replace the circle controller and issue the native path request.
        native.put(native.carrier + 0x68, 400 << 16)
        native.put(native.carrier + 0x6C, 0)
        native.put(native.carrier + 0x70, 160 << 16)
        native.placementResult = 1
        for retry_tick in range(4, 15):
            retry_row = native.dispatch(retry_tick)
        assert retry_row[0:2] == (1, 1), retry_row
        assert native.get(native.carrier + 0x60) == mission_identity
        assert native.get(native.carrier + 0xAC) == passenger_identity
        assert native.get(native.passenger + 0xA8) == carrier_identity
        assert native.requests == [1, 0, 1], native.requests
        assert len(native._controllerAllocations) == 2, native._controllerAllocations
        old_controller, controller_identity = native._controllerAllocations
        assert native.get(old_controller) == 0x5F28A4
        assert native.get(native.nav + 4) == controller_identity
        assert native.get(mission_identity + 0x6E) == controller_identity
        assert native.get(controller_identity + 4) == mission_identity
        assert native.controller_goal() == (0x5F28D8, expected_goal, 116), \
            native.controller_goal()
        position = tuple(struct.unpack('<3i', p.uc.mem_read(unit + 0x68, 12)))
        assert position == (start_x << 16, 0, start_z << 16), position

        # Match the route fixture's starting heading and physical Y seed without
        # replacing the carrier, mission, passenger, or controller.
        seed_x, seed_y, seed_z, seed_heading, seed_speed, seed_base, terrain_flags = world_seed
        assert (position[0], position[2]) == (seed_x, seed_z)
        assert terrain_flags == 0x1000 and seed_speed == 0
        p.uc.mem_write(unit + 0x6C, struct.pack('<i', seed_y))
        p.uc.mem_write(unit + 0x7E, struct.pack('<H', seed_heading))
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
        p.uc.mem_write(mover + 0x20, struct.pack('<i', seed_speed))
        p.uc.mem_write(unit + 0x12B, struct.pack('<i', seed_base))
        p.attach_live_request(unit, mover, nav_identity, controller_identity,
                              (sx - fx // 2, sz - fz // 2), (fx, fz))
        assert native.get(unit + 0x60) == mission_identity
        assert native.get(unit + 8) == mover_identity
        assert native.get(mover_identity) == nav_identity
        assert native.get(nav_identity + 4) == controller_identity
        assert native.get(nav_identity + 8) == unit
        assert native.get(mission_identity + 0x6E) == controller_identity
    else:
        p.plant_request(unit, (sx - fx // 2, sz - fz // 2), expected_goal)
        mover = struct.unpack('<I', p.uc.mem_read(unit + 8, 4))[0]
        p.uc.mem_write(unit + 0x68, struct.pack('<iii', start_x * 65536, 40 * 65536,
                                               start_z * 65536))
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
        p.uc.mem_write(unit + 0x7e, struct.pack('<H', heading))
        type_address = TYPE
    p.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
    p.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
    p.uc.mem_write(type_address + 0x18e, struct.pack('<H', turn))
    p.uc.mem_write(type_address + 0x172, struct.pack('<i', road))
    p.uc.mem_write(type_address + 0x260, struct.pack('<I', 0x80000))
    p.uc.mem_write(type_address + 0x16e, struct.pack('<i', water))
    p.uc.mem_write(type_address + 0x192, struct.pack('<hh', 10000, 13))
    if not integrated_retry:
        p.uc.mem_write(p.GRID + 4, struct.pack('<hh', fx, fz))

    query_count = 0

    def grade(_uc, args):
        nonlocal query_count
        x, z = struct.unpack('<ii', p.uc.mem_read(args, 8))
        query_count += 1
        value = grades[z * width + x] if 0 <= x < width and 0 <= z < height else 0
        return 3, value

    p.icd.hooks[0x4139d0] = grade
    if not integrated_retry:
        _, error = p.icd.call(0x4e2500,
            (p.HANDLE + 0x1000, target_x * 65536, target_z * 65536, 116), ecx=p.HANDLE)
        assert error is None, error
    goal = tuple(struct.unpack('<hh', p.uc.mem_read(p.HANDLE + 8, 4)))
    radius, radius_squared = struct.unpack('<ii', p.uc.mem_read(p.HANDLE + 0x0c, 8))
    assert (goal, radius, radius_squared) == (expected_goal, 116, 53), \
        (variant, goal, expected_goal, radius, radius_squared)

    _, error = p.init()
    assert error is None, error
    p.uc.mem_write(OBJ + 0x165, struct.pack('<I', 10_000_000))
    p.uc.mem_write(OBJ + 0x5c, struct.pack('<I', 1))
    _, error = p.step()  # retail reachability tracer
    assert error is None, error
    if variant != 7:
        assert p.phase() == 2, error

    completed = False
    for step in range(10_000):
        value, error = p.step()  # retail phase-2 cost search
        assert error is None, (step, error)
        if value:
            completed = True
            _, error = p.icd.call(0x414450, (0,), ecx=OBJ)
            assert error is None, error
            break
    assert completed, completed
    native_count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
    native_words = struct.unpack('<' + 'h' * (native_count * 2),
                                 p.uc.mem_read(p.NAV + 12, native_count * 4)) if native_count else ()
    native_route = list(zip(native_words[::2], native_words[1::2]))
    assert native_route[0] == anchor, (native_route[0], anchor)
    assert native_route[1:] == world_route, (native_route[1:], world_route)
    if physical_variant:
        from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ_UNMAPPED
        from unicorn.x86_const import UC_X86_REG_EIP
        def unmapped(uc, access, address, size, value, _data):
            print(f'NATIVE_STEP_UNMAPPED address={address:#x} eip={uc.reg_read(UC_X86_REG_EIP):#x}',
                  flush=True)
            return False
        p.uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED, unmapped)
        # Turn calls out to the unit's COB VM. This route kernel fixture has no
        # model/script, so suppress that unrelated callback while retaining the
        # original navigator steering and mover integration.
        p.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)
        trace = []
        def instruction(uc, address, size, _data):
            trace.append(address)
            if len(trace) > 100: del trace[0]
        p.uc.hook_add(UC_HOOK_CODE, instruction)
        # Phase's minimal gamestate leaves the global options object absent;
        # 4dc800's first access is [options+0x0a]. Supply a zeroed object to
        # expose the next real movement dependency without replacing movement.
        seed_x, seed_y, seed_z, seed_heading, seed_speed, seed_base, terrain_flags = world_seed
        assert terrain_flags == 0x1000 and seed_speed == 0
        # Cost-search's native grade hook receives World-computed footprint
        # scores above. The mover grid instead stores raw terrain grades and
        # applies the unit footprint itself. Reusing the scored plane here
        # double-applies the boat footprint at the shoreline and hides the
        # blocked edge that World::commitGroundStep sees.
        raw_grades = raw_water_grid(variant, width, height)
        for z in range(height):
            for x in range(width):
                p._set_grade(x, z, raw_grades[z * width + x])
        # Feed the real terrain plane into retail's mobile placement records.
        # Each native cell stores the max/min across its four corner samples;
        # copying one cell's height into both fields hid shoreline collisions.
        heights = raw_surface_heights(variant, width, height)
        cell_records = bytearray(width * height * 14)
        for z in range(height):
            for x in range(width):
                offset = (z * width + x) * 14
                samples = [heights[zz * width + xx]
                           for zz in (z, min(z + 1, height - 1))
                           for xx in (x, min(x + 1, width - 1))]
                cell_records[offset + 4:offset + 7] = bytes(
                    (heights[z * width + x], max(samples), min(samples)))
                struct.pack_into('<H', cell_records, offset + 8, 0xffff)
        p.uc.mem_write(CELLS, bytes(cell_records))
        # 507d10 reads the map's feature-type table even when all cells are
        # empty. Point it at a valid empty table instead of Phase's unit array.
        p.uc.mem_write(FEATURES, bytes(0x1000))
        p.uc.mem_write(GS + 0x19edc, struct.pack('<I', FEATURES))
        # The native mover derives its water multiplier from the height/global
        # sea-level pair before scanning. Hold the same explicit movement modes
        # on both sides for this arithmetic comparison; live scan parity needs a
        # full native map/body grid and is a separate boundary.
        p.uc.mem_write(GS + 0x19ef8, b'\x28')
        p.uc.mem_write(mover + 0x30, struct.pack('<I', tick + 1000))
        # 0x5066f0 updates the native 128-pixel body-sector index as the mover
        # crosses a bucket boundary. Phase has no scenario-created sector
        # buckets, so provide an empty native grid for this one carrier.
        sector_stride = (width + 7) // 8
        sector_rows = (height + 7) // 8
        sector_grid = p._alloc(sector_stride * sector_rows * 10)
        sector_records = bytearray(sector_stride * sector_rows * 10)
        for sector_z in range(sector_rows):
            for sector_x in range(sector_stride):
                block = [heights[z * width + x]
                         for z in range(sector_z * 8, min(height, sector_z * 8 + 8))
                         for x in range(sector_x * 8, min(width, sector_x * 8 + 8))]
                sector_records[(sector_z * sector_stride + sector_x) * 10 + 1] = max(block)
        p.uc.mem_write(sector_grid, bytes(sector_records))
        p.uc.mem_write(GS + 0x19f18, struct.pack('<I', sector_grid))
        p.uc.mem_write(GS + 0x19f1c, struct.pack('<I', sector_stride))
        p.uc.mem_write(GS + 0x600000, struct.pack('<I', GS + 0x700000))
        visibility = struct.unpack('<I', p.uc.mem_read(GS + 0x19ef4, 4))[0]
        p.uc.mem_write(visibility, struct.pack('<' + 'H' * (width * height // 4),
                                               *([0xffff] * (width * height // 4))))
        p.uc.mem_write(GS + 0x19f44, struct.pack('<I', tick + 1))
        p.uc.mem_write(unit + 0x68, struct.pack('<iii', seed_x, seed_y, seed_z))
        p.uc.mem_write(unit + 0x7e, struct.pack('<H', seed_heading))
        p.uc.mem_write(unit + 0x12b, struct.pack('<i', seed_base))
        p.uc.mem_write(mover + 0x20, struct.pack('<i', seed_speed))
        p.uc.mem_write(mover + 8, bytes(12)); p.uc.mem_write(mover + 0x14, bytes(12))
        # 0x4daf8f forwards mover +36's low bits to 507d10; bit 0 selects
        # its full mobile-footprint check for this active surface carrier.
        p.uc.mem_write(mover + 0x36, struct.pack('<H', flags | 1))
        p.uc.mem_write(type_address + 0x162, struct.pack('<i', seed_base))
        p.uc.mem_write(type_address + 0x166, struct.pack('<i', 1092))
        p.uc.mem_write(type_address + 0x16a, struct.pack('<i', 1092))
        p.uc.mem_write(type_address + 0x126, struct.pack('<hh', fx, fz))
        p.uc.mem_write(type_address + 0x18a, struct.pack('<I', p.GRID))
        p.uc.mem_write(type_address + 0x23c, bytes((255, 255)))
        p.uc.mem_write(type_address + 0x24a, b'\x01')
        p.uc.mem_write(p.GRID + 8, struct.pack('<4h4B', 10000, 13, 10000, 13, 255, 127, 255, 127))
        before = struct.unpack('<iii', p.uc.mem_read(unit + 0x68, 12))
        world_mission = tuple(map(int, next(
            line for line in stdout if line.startswith('WORLDMISSION ')).split()[1:]))
        (mission_stage, mission_wait, mission_deadline, mission_pending,
         mission_flags, approach_attempts, cargo_count, passenger_id,
         target_x_fixed, target_z_fixed) = world_mission
        assert (mission_stage, cargo_count, passenger_id) == (1, 1, 2), world_mission
        if integrated_retry:
            mission, passenger, owner = native.mission, native.passenger, native.owner
        else:
            mission = p.HANDLE + 0x1000
            passenger = UNITS + 2 * 0x140
            owner = struct.unpack('<I', p.uc.mem_read(unit + 0xb8, 4))[0]
        passenger_type = p._alloc(0x400)
        if not integrated_retry:
            p.uc.mem_write(passenger, bytes(0x140))
        p.uc.mem_write(passenger_type, bytes(0x400))
        p.uc.mem_write(passenger + 2, struct.pack('<H', 2))
        p.uc.mem_write(passenger + 0xb4, struct.pack('<I', passenger_type))
        p.uc.mem_write(passenger + 0xb8, struct.pack('<I', owner))
        p.uc.mem_write(passenger + 0xa8, struct.pack('<I', unit))
        p.uc.mem_write(passenger + 0x130, struct.pack('<I', 0x1000000))
        p.uc.mem_write(unit + 0xac, struct.pack('<I', passenger))
        p.uc.mem_write(unit + 0x60, struct.pack('<I', mission))
        p.uc.mem_write(unit + 0xa4, bytes(4))
        p.uc.mem_write(unit + 0x130, struct.pack('<I', 0x1000000))
        p.uc.mem_write(unit + 0xb8, struct.pack('<I', owner))
        p.uc.mem_write(type_address + 0x23e, struct.pack('<H', 150))
        p.uc.mem_write(type_address + 0x260, struct.pack('<I', 0))
        p.uc.mem_write(type_address + 0x264, struct.pack('<I', 0x200))
        p.uc.mem_write(type_address + 0x14a, struct.pack('<I', 1 << 16))
        p.uc.mem_write(type_address + 0x24b, b'\0')
        p.uc.mem_write(owner + 0xea, b'\x01')
        p.uc.mem_write(owner + 0x74, struct.pack('<I', ARENA + 0x1500000))
        p.uc.mem_write(owner + 0x78, struct.pack('<I', ARENA + 0x14fffff))
        definitions = p._alloc(0x100)
        p.uc.mem_write(definitions + 25 + 4, struct.pack('<I', 0x408d50))
        p.uc.mem_write(0x62d55c, struct.pack('<I', GS))
        p.uc.mem_write(0x62db84, struct.pack('<I', definitions))
        p.uc.mem_write(0x64186c, struct.pack('<I', tick))
        p.uc.mem_write(GS + 0x19f30, struct.pack('<I', 1))
        p.uc.mem_write(GS + 0x174c8, struct.pack('<I', 101))
        p.uc.mem_write(GS + 0x174cc, struct.pack('<I', 102))
        if integrated_retry:
            native_stage = p.uc.mem_read(mission + 5, 1)[0]
            assert native_stage == mission_stage, (native_stage, world_mission)
            assert native.get(mission + 6) == mission_wait, (native.get(mission + 6), world_mission)
            native.put(mission + 0x0A, mission_deadline)
            assert native.get(mission + 0x0E) == unit
            assert native.get(mission + 0x16) == passenger
            assert native.get(mission + 0x6E) == p.HANDLE
        else:
            p.uc.mem_write(mission, bytes(0x72))
            p.uc.mem_write(mission + 4, b'\x01')
            p.uc.mem_write(mission + 5, bytes((mission_stage,)))
            p.uc.mem_write(mission + 6, struct.pack('<I', mission_wait))
            p.uc.mem_write(mission + 0x0a, struct.pack('<I', mission_deadline))
            p.uc.mem_write(mission + 0x0e, struct.pack('<I', unit))
            p.uc.mem_write(mission + 0x16, struct.pack('<I', passenger))
            p.uc.mem_write(mission + 0x22, struct.pack('<i', target_x_fixed))
            p.uc.mem_write(mission + 0x26, struct.pack('<i', 0))
            p.uc.mem_write(mission + 0x2a, struct.pack('<i', target_z_fixed))
            p.uc.mem_write(mission + 0x52, struct.pack('<H', approach_attempts))
            # The World's 0x1000 bit marks the pending path-service notification;
            # the native dispatcher consumes it as an engine event before checking
            # this mission's wait mask, so it is not a transport-handler event.
            p.uc.mem_write(mission + 0x6a, struct.pack('<I', mission_pending & 0x700))
            p.uc.mem_write(mission + 0x6e, struct.pack('<I', p.HANDLE))

        native_effects = 0
        native_parked = False
        native_requests = native.requests if integrated_retry else []
        def detach(_uc, _sp):
            p.uc.mem_write(passenger + 0xa8, bytes(4))
            p.uc.mem_write(unit + 0xac, bytes(4))
            return 5, 0
        def effect(_uc, _sp):
            nonlocal native_effects
            native_effects += 1
            return 2, 0
        def park(_uc, _sp):
            nonlocal native_parked
            native_parked = True
            return 11, 0
        def place(_uc, _sp):
            return 5, 1
        def request(_uc, sp):
            address = struct.unpack('<I', _uc.mem_read(sp, 4))[0]
            native_requests.append(address)
            return 1, 0
        def bind_reference(uc, sp):
            from unicorn.x86_const import UC_X86_REG_ECX
            reference = uc.reg_read(UC_X86_REG_ECX)
            address = struct.unpack('<I', uc.mem_read(sp, 4))[0]
            uc.mem_write(reference + 4, struct.pack('<I', address))
            return 1, reference
        if integrated_retry:
            # These are the remaining fixture boundaries for the shared-Icd
            # path: no ship COB, and no detailed hull mesh callback.
            p.icd.hooks[0x51ad20] = lambda _uc, _sp: (1, 0)
            p.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)
        else:
            p.icd.hooks.update({
                0x415f30: lambda _uc, _sp: (1, 0),
                0x4d4bf0: lambda _uc, _sp: (1, 0),
                0x5199f0: bind_reference,
                0x4d6ad0: lambda _uc, _sp: (2, 0),
                0x4f5db0: lambda _uc, _sp: (2, 0),
                0x50a9c0: lambda _uc, _sp: (3, 0),
                0x4e4f50: request,
                0x507d10: place,
                # The fixture omits the ship type's detailed hull/waterline mesh.
                # Keep the seeded Y fixed while comparing native route movement and
                # map-footprint placement, which use the real height plane above.
                0x51ad20: lambda _uc, _sp: (1, 0),
                0x421e10: effect,
                0x51b4f0: detach,
                0x4d78a0: park,
            })
        if integrated_retry:
            p.icd.freeze_hooks()
        put = lambda address, value: p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
        world_trans = {int(fields[0]): tuple(map(int, fields[1:]))
                       for fields in (line.split()[1:] for line in stdout
                                     if line.startswith('WORLDTRANS '))}
        assert len(world_trans) == len(world_steps), (len(world_trans), len(world_steps))
        assert world_steps and len(world_nav) == len(world_steps), (
            len(world_steps), len(world_nav), len(world_points))
        native_callback_counts = {0x4e5150: 0, 0x4e50a0: 0, 0x507d10: 0}
        if integrated_retry:
            native_callback_counts[0x4d6ad0] = 0
        native_placement_arguments = []
        from unicorn import UC_HOOK_CODE
        from unicorn.x86_const import UC_X86_REG_ESP
        def route_callback(uc, address, _size, _data):
            if address in native_callback_counts:
                native_callback_counts[address] += 1
                if address == 0x507d10:
                    stack = uc.reg_read(UC_X86_REG_ESP)
                    native_placement_arguments.append(
                        struct.unpack('<5I', uc.mem_read(stack + 4, 20)))
        p.uc.hook_add(UC_HOOK_CODE, route_callback)
        route_transition_steps = []
        prior_native_route = native_route
        movement_trace = []
        mission_retired = False
        if integrated_retry:
            assert native.get(unit + 0x60) == mission_identity
            assert native.get(unit + 8) == mover_identity
            assert native.get(mover_identity) == nav_identity
            assert native.get(nav_identity + 4) == controller_identity
            assert native.get(mission_identity + 0x6e) == controller_identity
        for step, row in enumerate(world_steps, 1):
            placement_start = len(native_placement_arguments)
            p.uc.mem_write(GS + 0x19f44, struct.pack('<I', tick + step))
            try:
                if integrated_retry:
                    native.dispatch(tick + step)
                    active_after_dispatch = native.get(unit + 0x60)
                    if active_after_dispatch == 0:
                        mission_retired = True
                    assert active_after_dispatch == (0 if mission_retired else mission_identity)
                else:
                    value, error = p.icd.call(0x4d8450, (unit,))
                    assert error is None, error
                placement_hooks = p.icd.hooks
                p.icd.hooks = {address: hook for address, hook in placement_hooks.items()
                               if address != 0x507d10}
                try:
                    value, error = p.icd.call(0x4dc800, (unit,), ecx=mover)
                finally:
                    p.icd.hooks = placement_hooks
                assert error is None, error
                value, error = p.icd.call(0x51b2a0, (unit,), ecx=mover)
                assert error is None, error
            except Exception as exc:
                if 'placement_hooks' in locals():
                    p.icd.hooks = placement_hooks
                print('NATIVE_STEP_ERROR', step, exc,
                      'trace', ' '.join(hex(a) for a in trace[-30:]))
                raise
            move_flags = struct.unpack('<H', p.uc.mem_read(mover + 0x36, 2))[0]
            native_step = (*struct.unpack('<iii', p.uc.mem_read(unit + 0x68, 12)),
                           struct.unpack('<H', p.uc.mem_read(unit + 0x7e, 2))[0],
                           struct.unpack('<i', p.uc.mem_read(mover + 0x20, 4))[0],
                           (move_flags >> 5) & 7, (move_flags >> 8) & 7,
                           move_flags & 0x1800)
            expected_step = (*row[1:6], *row[6:9])
            if native_step != expected_step:
                print(f'SURFACE_STEP_DIVERGENCE step={step} native={native_step} '
                      f'world={expected_step} world_turn_request={row[9]} '
                      f'native_flags={move_flags:#06x}', flush=True)
            movement_trace.append((step, native_step, expected_step))
            if len(movement_trace) > 12:
                movement_trace.pop(0)
            world_state = world_nav[step]
            (world_count, world_events, world_stamp, world_controller,
             world_pending, world_wait, world_stage, _front_x, _front_z,
             _segment_x, _segment_z, world_unload, world_goal, world_has_segment,
             world_orders, world_consumed, world_exhausted) = world_state
            count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
            words = struct.unpack('<' + 'h' * (count * 2),
                                  p.uc.mem_read(p.NAV + 12, count * 4)) if count else ()
            route = list(zip(words[::2], words[1::2]))
            controller = struct.unpack('<I', p.uc.mem_read(p.NAV + 4, 4))[0]
            mission_event_address = mission + 0x6a if integrated_retry else p.HANDLE + 0x1000 + 0x6a
            mission_events = struct.unpack('<I', p.uc.mem_read(mission_event_address, 4))[0]
            native_mission = (
                int(struct.unpack('<I', p.uc.mem_read(unit + 0x60, 4))[0] == mission),
                p.uc.mem_read(mission + 5, 1)[0],
                struct.unpack('<I', p.uc.mem_read(mission + 6, 4))[0],
                struct.unpack('<I', p.uc.mem_read(mission + 0x0a, 4))[0],
                struct.unpack('<I', p.uc.mem_read(mission + 0x6a, 4))[0] & 0x700,
                int(struct.unpack('<I', p.uc.mem_read(unit + 0xac, 4))[0] != 0),
                int(struct.unpack('<I', p.uc.mem_read(passenger + 0xa8, 4))[0] == unit))
            wt = world_trans[step]
            (world_orders, world_cargo, world_stage, world_wait, world_deadline,
             world_pending, world_flags, world_attempts, world_ticks,
             world_embarked, world_effects) = wt
            # World keeps arrival/detachment 0x500 in mission.pending, while
            # retail posts the equivalent bits on the unit event word and the
            # dispatcher consumes them on the next pass.
            comparable_pending = world_pending & 0x700
            if variant == 8:
                comparable_pending &= ~0x500
                native_mission = (*native_mission[:4], native_mission[4] & ~0x500,
                                  *native_mission[5:])
            expected_mission = (int(world_unload), world_stage, world_wait, world_deadline,
                                comparable_pending, int(world_cargo != 0), world_embarked)
            if native_mission[0] or expected_mission[0]:
                assert native_mission == expected_mission, (
                'sea unload dispatcher/cargo state', step, native_mission,
                expected_mission, wt)
            if integrated_retry:
                active_mission = native.get(unit + 0x60)
                assert active_mission in (mission_identity, 0), (step, hex(active_mission))
                assert native.get(unit + 8) == mover_identity
                assert native.get(mover_identity) == nav_identity
                assert native.get(nav_identity + 8) == unit
                active_controller = native.get(nav_identity + 4)
                assert active_controller in (controller_identity, 0), (
                    step, hex(active_controller), hex(controller_identity))
                stored_controller = native.get(mission_identity + 0x6E)
                assert stored_controller in (controller_identity, 0), (
                    step, hex(stored_controller), hex(controller_identity))
                if active_mission:
                    assert native.get(mission_identity + 0x0E) == unit
                    assert native.get(mission_identity + 0x16) == passenger_identity
                    assert bytes(p.uc.mem_read(mission_identity + 0x22, 12)) == original_destination
                else:
                    assert mission_retired and not active_controller and not stored_controller
                carrier_cargo = native.get(unit + 0xAC)
                passenger_carrier = native.get(passenger_identity + 0xA8)
                assert ((carrier_cargo == passenger_identity and
                         passenger_carrier == carrier_identity) or
                        (carrier_cargo == 0 and passenger_carrier == 0)), (
                    step, hex(carrier_cargo), hex(passenger_carrier))
                if carrier_cargo == 0:
                    released_position = tuple(struct.unpack('<3i',
                        p.uc.mem_read(passenger_identity + 0x68, 12)))
                    assert released_position == (500 << 16, 0, 500 << 16), (
                        step, released_position)
                if active_mission == 0:
                    mission_retired = True
                else:
                    assert not mission_retired, step
            if native_step != expected_step:
                for prior in movement_trace:
                    print('SURFACE_RECENT', *prior, flush=True)
                print('SURFACE_MISSION', step, native_mission, expected_mission, wt, flush=True)
                debug_count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
                debug_words = struct.unpack('<' + 'h' * (debug_count * 2),
                                            p.uc.mem_read(p.NAV + 12, debug_count * 4)) if debug_count else ()
                debug_controller = struct.unpack('<I', p.uc.mem_read(p.NAV + 4, 4))[0]
                debug_goal = tuple(struct.unpack('<hh', p.uc.mem_read(debug_controller + 8, 4))) if debug_controller else ()
                debug_radius = struct.unpack('<ii', p.uc.mem_read(debug_controller + 0x0c, 8)) if debug_controller else ()
                print('SURFACE_NAV', step, debug_count, list(zip(debug_words[::2], debug_words[1::2])),
                      hex(debug_controller), debug_goal, debug_radius,
                      world_nav[step], world_points.get(step, []), 'grade_queries', query_count,
                      'callbacks', native_callback_counts, 'placement_calls',
                      native_placement_arguments[placement_start:],
                      'requests', len(native_requests),
                      flush=True)
            assert native_step == expected_step, ('surface unload native movement step',
                                                   step, native_step, expected_step, world_seed)
            active_goal = tuple(struct.unpack('<hh', p.uc.mem_read(controller + 8, 4))) if controller else ()
            active_radius, active_radius_squared = struct.unpack('<ii', p.uc.mem_read(controller + 0x0c, 8)) if controller else (0, 0)
            if world_orders:
                assert count == world_count + 1, ('sea route cursor count', step, count, world_count)
                expected_points = world_points.get(step, [])
                assert route[1:] == expected_points, ('sea route cursor points', step, route, expected_points)
                assert bool(controller) == bool(world_controller), ('sea unload controller active', step, controller, world_controller)
                if controller:
                    assert (active_goal, active_radius, active_radius_squared) == (
                        goal, radius, radius_squared), ('sea unload circle changed during segment travel', step,
                                                          active_goal, active_radius, active_radius_squared)
                    assert bool(p.uc.mem_read(p.NAV + 0x114, 1)[0] & 1) == bool(world_has_segment), (
                        'sea route segment-valid bit', step, p.uc.mem_read(p.NAV + 0x114, 1)[0], world_has_segment)
                else:
                    assert (active_goal, active_radius, active_radius_squared) == ((), 0, 0), (
                        'detached sea unload circle retained a native controller', step,
                        active_goal, active_radius, active_radius_squared)
                if world_stage == 1:
                    arrival_leads_dispatch = (variant == 8 and
                        mission_events & 0x100 and not world_events & 0x100 and
                        world_state[6] == 1)
                    assert arrival_leads_dispatch or (
                        mission_events & 0x100 == world_events & 0x100 == 0), (
                            'intermediate waypoint raised final circle-arrival event', step,
                            hex(mission_events), hex(world_events), native_step,
                            route, world_state, world_points.get(step, []))
                    assert world_unload == 1 and world_goal == 1 and (
                        world_controller == 1 or arrival_leads_dispatch), (
                        'unload approach controller state', step, world_state)
                    assert (world_pending == 4096 or (variant == 8 and
                            world_pending == 0x1500 and arrival_leads_dispatch)) and world_wait == 1793, (
                                'unload pending wait mask', step, world_state)
                    assert world_count == len(expected_points) and world_orders == world_count + 1, (
                        'World route cursor/order size', step, world_state, expected_points)
                elif world_stage >= 2:
                    assert world_unload == 1 and world_goal == 1, (
                        'surface transfer must remain on its routed unload goal', step, world_state)
                    if world_controller:
                        assert world_orders == world_count, (
                            'active transfer route should own its combined unload goal', step, world_state)
                else:
                    assert world_stage == 0 and world_trans[step][1] == 0, (
                        'completed surface transfer must retain only its one-tick release tail',
                        step, world_state, world_trans[step])
                    assert world_pending & 0x500 == 0x500 and not world_controller, (
                        'surface unload release tail events/controller', step, world_state)
            else:
                assert not world_unload and not world_controller and native_mission[0] == 0, (
                    'surface unload mission should be released after the coasting tail',
                    step, world_state, native_mission)
            if count != len(prior_native_route):
                route_transition_steps.append(step)
                assert count + 1 == len(prior_native_route), ('native route count skipped', step, prior_native_route, route)
                assert route[0] == prior_native_route[1], ('native route cursor did not advance one point', step, prior_native_route, route)
            prior_native_route = route
        assert route_transition_steps, \
            f'{len(world_steps)} physical steps did not cross a native route waypoint'
        transfer_route_steps = [step for step, state in world_nav.items()
                                if state[6] >= 2 and state[3]]
        if variant == 8:
            assert not transfer_route_steps, transfer_route_steps
            release_steps = [step for step, state in world_trans.items()
                             if state[0] and not state[1]]
            retirement_steps = [step for step, state in world_trans.items()
                                if not state[0] and step > 1 and world_trans[step - 1][0]]
            assert len(release_steps) == len(retirement_steps) == 1, (
                release_steps, retirement_steps)
            assert retirement_steps[0] == release_steps[0] + 1, (
                release_steps, retirement_steps)
            if integrated_retry:
                assert mission_retired, 'the original retry mission was not retired'
                assert native.get(unit + 0x60) == 0
                assert native.get(unit + 0xAC) == 0
                assert native.get(passenger_identity + 0xA8) == 0
                assert native_callback_counts[0x4d6ad0] >= 1, native_callback_counts
        else:
            assert transfer_route_steps, \
                'World did not keep the surface navigator active while unload transfer ran'
        assert native_callback_counts[0x4e5150] == len(world_steps), native_callback_counts
        assert native_callback_counts[0x4e50a0] == len(route_transition_steps), (
            native_callback_counts, route_transition_steps)
        callback_summary = ', '.join(f'{address:#x}:{count}'
                                     for address, count in native_callback_counts.items())
        if variant == 8:
            circle_detach = next((step for step, state in world_nav.items()
                                  if state[6] == 1 and not state[3] and
                                  state[4] & 0x500 == 0x500), None)
            trace_label = ('one native same-trip retry mission/carrier/passenger/controller '
                           'through retail search, mover, release and retirement; ' if integrated_retry else '')
            print(f'PASS: {trace_label}retail 0x4dc800+0x51b2a0 matches {len(world_steps)} World movement steps '
                  f'from remote post-retry seed {world_seed}; the replacement route detaches at '
                  f'physical step {circle_detach}, passenger release/mission retirement at '
                  f'{release_steps[0]}/{retirement_steps[0]}, route-point transitions at '
                  f'{route_transition_steps}; callbacks={{{callback_summary}}}; last={native_step}')
        else:
            print(f'PASS: retail 0x4dc800+0x51b2a0 matches {len(world_steps)} World movement steps, '
                  f'active route state, and the unload transfer/coast boundary '
                  f'from delayed route seed {world_seed}; transfer-route overlap at '
                  f'{transfer_route_steps[0]}..{transfer_route_steps[-1]}, circle arrival at physical step '
                  f'{next((step for step, state in world_nav.items() if state[6] >= 2 and not state[3]), None)}; route-point transitions at '
                  f'{route_transition_steps}; callbacks={{{callback_summary}}}; last={native_step}')
    if variant == 7:
        assert world_failures == 1 and world_completions == 0, route_header
        print(f'PASS: unreachable boat unload variant {variant} returns the same partial '
              f'route after World failure at tick {tick}; {len(world_route)} waypoint, '
              f'{query_count} native grade queries')
    elif not integrated_retry:
        print(f'PASS: boat circle route variant {variant} matches exactly at tick {tick}; '
              f'heading {heading}, native including anchor={native_route}, {query_count} grade queries')
    return {
        'seed': world_seed,
        'target': (target_x * 65536, target_z * 65536),
        'native_route': native_route,
        'world_route': world_route,
        'physical_steps': len(world_steps),
        'circle_radius': radius,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('world_binary', nargs='?', default='build-o2/transport_test')
    parser.add_argument('--variant', type=int, choices=range(9), action='append')
    parser.add_argument('--steps', type=int, default=1000,
                        help='physical movement steps for variants 0/8 (1..1000; includes unload transfer)')
    args = parser.parse_args()
    if not 1 <= args.steps <= 1000:
        parser.error('--steps must be between 1 and 1000')
    for case in args.variant if args.variant is not None else range(8):
        check_variant(args.world_binary, case, args.steps)
