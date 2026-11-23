#!/usr/bin/env python3
"""Trace VTOL pickup with a secondary-list passenger pickup request.

This explicitly sets flag 0x20000, putting code 30 Move_Seek_Pickup on
unit+0x64 while code 27 Move_Ground occupies the primary unit+0x60 list.
It does NOT reproduce ordinary Shift-queue append (4d77f0, primary-list tail).
For each passenger update it runs retail's active dispatcher 0x4d8450
followed by secondary dispatcher 0x4d85e0. At tick 4, VTOL_Pickup
retires because code 27 is still current; queued code 30 is dispatched,
returns 8, and is removed before the move completes. The trace stops at that
native retirement result and does not claim a later boarding sequence.
Controlled headless boundaries are path-worker scheduling callbacks,
visibility, feature definitions, effects/UI, and empty COB method tables. No
retail GUI is launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_ground_move_seek.py
"""
import argparse
import hashlib
import pickle
import re
import struct
from pathlib import Path

from balance_inputs import class_record, properties, unit_properties
from check_surface_unload_map_grades import native_grade_reader, asset as retail_asset
from check_surface_unload_map_release import cat
from emuphase import GS, OBJ
from probe_air_pickup_native_fullmap import (
    DEFAULT_CARRIER, DEFAULT_PASSENGER, START, TARGET, SCALE,
    make_map, native_half_cell_ticks,
)
from probe_air_pickup_native_moving_target import (
    unit_profile,
)
from probe_surface_pickup_native_fullmap import install_native_entity_array
from probe_surface_pickup_native_map_route import run as run_route
from probe_transport_air_unload_map_flight import carrier_profile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
    UC_X86_REG_ESI, UC_X86_REG_ESP,
)


def put(uc, address, *values):
    uc.mem_write(address, struct.pack("<" + "I" * len(values),
                                      *(value & 0xFFFFFFFF for value in values)))


def byte(uc, address, value):
    uc.mem_write(address, bytes((value & 0xFF,)))


def grade2_profile(hpitool, root, passenger_name):
    moveinfo = cat(hpitool, root, "data.hpi",
                   "gamedata/moveinfo.tdf").decode("latin1")
    fbi = cat(hpitool, root, "data.hpi",
              f"units/{passenger_name}.fbi").decode("latin1")
    unit_fields = unit_properties(fbi)
    movement = unit_fields["movementclass"].lower()
    if movement != "ground2":
        raise AssertionError((passenger_name, movement, "expected GROUND2"))
    body = next(properties(block) for block in
                re.findall(r"\[[^]]+\]\s*\{([^{}]*)\}", moveinfo, re.S)
                if properties(block).get("name", "").lower() == movement)
    profile = class_record(body)
    fx, fz = struct.unpack_from("<hh", profile)
    if (fx, fz) != (2, 2):
        raise AssertionError(("shipped Araarch GROUND2 footprint changed", fx, fz))
    return unit_fields, profile, fx, fz


def ground_grade_plane(map_data, profile):
    width, height = map_data[:2]
    digest = hashlib.sha256(
        profile + bytes(map_data[3]) +
        struct.pack("<" + "H" * len(map_data[4]), *map_data[4])).hexdigest()
    cache = Path("/tmp") / f"tak-lokken-ground2-{digest}.pkl"
    if cache.exists():
        grades = pickle.loads(cache.read_bytes())
        if len(grades) != width * height:
            raise AssertionError(("cached GROUND2 grades shape", len(grades), width, height))
        return grades, cache
    print(f"grading {width * height} GROUND2 cells through native 0x508cd0...",
          flush=True)
    native_grade = native_grade_reader(map_data, profile)
    grades = [native_grade(x, z) for z in range(height) for x in range(width)]
    cache.write_bytes(pickle.dumps(grades))
    return grades, cache


def register_orders(phase, carrier, passenger, carrier_name, passenger_name,
                    move_goal):
    uc, icd = phase.uc, phase.icd
    get = lambda address: struct.unpack("<I", uc.mem_read(address, 4))[0]
    putter = lambda address, *values: put(uc, address, *values)
    byte(uc, 0x62DB80, 0)
    putter(0x62DB84, 0, 0, 0)
    for registration in (0x402740, 0x4092E0, 0x421850):
        _, error = icd.call(registration)
        if error:
            raise RuntimeError(("native mission descriptor registration",
                                hex(registration), error))
    descriptors, descriptor_end = get(0x62DB84), get(0x62DB88)
    if (descriptor_end - descriptors) // 25 != 76:
        raise AssertionError(("mission descriptor count",
                              (descriptor_end - descriptors) // 25))

    hooks = dict(icd.hooks)
    hooks.pop(0x4D4BF0, None)
    icd.hooks = hooks

    def lookup(name_address):
        result = phase._alloc(4)
        _, error = icd.call(0x4D4BF0, (name_address,), ecx=result)
        if error:
            raise RuntimeError(("native mission-name lookup", hex(name_address), error))
        code = uc.mem_read(result, 1)[0]
        if not code:
            raise AssertionError(("mission name not registered", hex(name_address)))
        row = descriptors + code * 25
        name_ptr = get(row + 0x15)
        name = bytes(uc.mem_read(name_ptr, 80)).split(b"\0")[0].lower()
        return code, get(row + 4), name

    vtol = lookup(0x604DE4)
    if vtol != (62, 0x41A680, b"vtol_pickup"):
        raise AssertionError(("retail VTOL_Pickup registry", vtol))
    pickup_row = descriptors + 30 * 25
    pickup_name = get(pickup_row + 0x15)
    pickup = (30, get(pickup_row + 4),
              bytes(uc.mem_read(pickup_name, 80)).split(b"\0")[0].lower())
    if pickup != (30, 0x403430, b"move_seek_pickup"):
        raise AssertionError(("retail Move_Seek_Pickup registry", pickup))
    move_row = descriptors + 27 * 25
    move_name = get(move_row + 0x15)
    move = (27, get(move_row + 4),
            bytes(uc.mem_read(move_name, 80)).split(b"\0")[0].lower())
    if move != (27, 0x402B00, b"move_ground"):
        raise AssertionError(("retail Move_Ground registry", move))

    # Remove the previous synthetic route fixture's carrier/passenger pair.
    carrier_old, passenger_old = get(carrier + 0x60), get(passenger + 0x60)
    if carrier_old and get(carrier_old + 0x6E):
        _, error = icd.call(0x4D4D40, (0,), ecx=carrier_old)
        if error:
            raise RuntimeError(("detach prior surface route controller", error))
    if passenger_old and get(passenger_old + 0x6E):
        _, error = icd.call(0x4D4D40, (0,), ecx=passenger_old)
        if error:
            raise RuntimeError(("detach prior passenger controller", error))
    putter(carrier + 0x60, 0)
    putter(passenger + 0x60, 0)
    putter(carrier + 0x64, 0)
    putter(passenger + 0x64, 0)
    putter(carrier + 0xC4, 0)
    putter(passenger + 0xC4, 0)

    carrier_order, passenger_order, seek_order = (
        phase._alloc(0x100), phase._alloc(0x100), phase._alloc(0x100))
    goal = phase._alloc(12)
    uc.mem_write(goal, struct.pack("<iii", *move_goal))

    def construct(code, target, order):
        result, error = icd.call(
            0x4D6C40, (code, 0 if code == 27 else target,
                       target if code == 27 else 0,
                       0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=order)
        if error or result != order:
            raise RuntimeError(("native transport/move order constructor",
                                code, result, error))
        if uc.mem_read(order + 4, 1)[0] != code:
            raise AssertionError(("native order constructor mission code",
                                  code, uc.mem_read(order + 4, 1)[0]))
        if code == 27 and struct.unpack("<3i", uc.mem_read(order + 0x22, 12)) != \
                struct.unpack("<3i", uc.mem_read(target, 12)):
            raise AssertionError(("native Move_Ground point constructor",
                                  struct.unpack("<3i", uc.mem_read(order + 0x22, 12)),
                                  struct.unpack("<3i", uc.mem_read(target, 12))))

    def insert(unit, order, label):
        _, error = icd.call(0x4D7750, (unit, order))
        if error:
            raise RuntimeError(("native transport/move order insertion", label, error))

    # Install the point move first as the active mission. Retail's 0x20000
    # secondary-list flag sends code 30 to unit+0x64. This is concurrent
    # dispatch, not ordinary append behind the move on the primary chain.
    construct(27, goal, passenger_order)
    insert(passenger, passenger_order, "Move_Ground")
    construct(30, carrier, seek_order)
    putter(seek_order + 0x5A, get(seek_order + 0x5A) | 0x20000)
    insert(passenger, seek_order, "queued Move_Seek_Pickup")
    construct(62, passenger, carrier_order)
    insert(carrier, carrier_order, "VTOL_Pickup")
    if (get(carrier + 0x60), get(passenger + 0x60)) != \
            (carrier_order, passenger_order):
        raise AssertionError("native pickup order heads were not installed")
    if get(passenger + 0x64) != seek_order or get(seek_order + 0x66):
        raise AssertionError(("native 0x20000 insertion did not install secondary code 30",
                              hex(get(passenger + 0x64)), hex(seek_order),
                              hex(get(seek_order + 0x66))))
    if get(carrier + 0xC4) != seek_order + 0x12 or \
            get(passenger + 0xC4) != carrier_order + 0x12:
        raise AssertionError(("native pickup target references",
                              hex(get(carrier + 0xC4)),
                              hex(get(passenger + 0xC4)),
                              hex(seek_order + 0x12),
                              hex(carrier_order + 0x12)))
    return carrier_order, passenger_order, seek_order


def set_unit_types_and_movers(phase, live, map_data, sectors, stride,
                              carrier_profile_data, passenger_fields,
                              movement_profile, fx, fz):
    uc = phase.uc
    width, _height, sea, heights = map_data[:4]
    carrier, passenger, owner = live.carrier, live.passenger, live.owner

    # ZONROC gets its own authored flying type/mover record. The carrier flight
    # navigator and the passenger's ground navigator remain independent.
    carrier_kind = phase._alloc(0x400)
    passenger_kind = phase._alloc(0x400)
    carrier_mask = phase._alloc(0x1000)
    passenger_mask = phase._alloc(0x1000)
    put(uc, carrier_kind + 0x12A, carrier_mask)
    put(uc, passenger_kind + 0x12A, passenger_mask)
    put(uc, carrier + 0xB4, carrier_kind)
    put(uc, passenger + 0xB4, passenger_kind)
    put(uc, carrier + 0xB8, owner)
    put(uc, passenger + 0xB8, owner)

    flight_mover, flight_nav = phase._alloc(0x80), phase._alloc(0x180)
    put(uc, flight_nav, 0x5F34D4, 0, carrier)
    put(uc, flight_mover, flight_nav)
    put(uc, flight_mover + 0x20, 0)
    put(uc, flight_mover + 0x30, 0)
    uc.mem_write(flight_mover + 0x36, struct.pack("<H", 2))
    put(uc, carrier + 8, flight_mover)

    profile = carrier_profile_data
    carrier_footprint = profile["footprintx"] | (profile["footprintz"] << 16)
    put(uc, carrier + 0x12B, int(float(profile["maxvelocity"]) * 65536))
    put(uc, carrier_kind + 0x162, int(float(profile["maxvelocity"]) * 65536))
    put(uc, carrier_kind + 0x166, int(float(profile["brakerate"]) * 65536))
    put(uc, carrier_kind + 0x16A, int(float(profile["acceleration"]) * 65536))
    put(uc, carrier_kind + 0x16E, int(float(profile["watermultiplier"]) * 65536))
    put(uc, carrier_kind + 0x172, int(float(profile["roadmultiplier"]) * 65536))
    put(uc, carrier_kind + 0x182, 1 << 16)
    put(uc, carrier_kind + 0x186, 9 << 16)
    uc.mem_write(carrier_kind + 0x18E, struct.pack("<H", profile["turnrate"]))
    uc.mem_write(carrier_kind + 0x23A, struct.pack("<h", profile["cruisealt"]))
    uc.mem_write(carrier_kind + 0x23E,
                 struct.pack("<H", profile["transportdistance"]))
    byte(uc, carrier_kind + 0x249, native_half_cell_ticks(profile))
    put(uc, carrier_kind + 0x126, carrier_footprint)
    put(uc, carrier_kind + 0x260, 0x800)
    put(uc, carrier_kind + 0x264, 0x200)
    put(uc, carrier + 0x78, carrier_footprint)
    sx, sz = START
    sy = heights[sz * width + sx] + profile["cruisealt"]
    put(uc, carrier + 0x68, sx * SCALE * 65536, sy * 65536,
        sz * SCALE * 65536)
    uc.mem_write(carrier + 0x74, struct.pack("<hh", sx, sz))
    put(uc, flight_nav + 0x0C, sx * SCALE * 65536, sy * 65536,
        sz * SCALE * 65536)
    put(uc, flight_nav + 0x24, 0)

    # Shipped Araarch's GROUND2 movement inputs and 2x2 footprint.
    passenger_footprint = fx | (fz << 16)
    put(uc, passenger_kind + 0x126, passenger_footprint)
    put(uc, passenger_kind + 0x18A, phase.GRID)
    put(uc, passenger_kind + 0x18E,
        int(float(passenger_fields.get("turnrate", 500))))
    put(uc, passenger_kind + 0x172,
        int(float(passenger_fields.get("roadmultiplier", 1.0)) * 65536))
    put(uc, passenger_kind + 0x16E,
        int(float(passenger_fields.get("watermultiplier", 1.0)) * 65536))
    put(uc, passenger_kind + 0x14A, 1 << 16)
    put(uc, passenger_kind + 0x260, 0)
    put(uc, passenger_kind + 0x264, 0x200)
    for key, offset, default in (("maxvelocity", 0x162, 0),
                                 ("brakerate", 0x166, 0.5),
                                 ("acceleration", 0x16A, 0.5)):
        put(uc, passenger_kind + offset,
            int(float(passenger_fields.get(key, default)) * 65536))
    uc.mem_write(passenger_kind + 0x192, movement_profile[4:12])
    uc.mem_write(passenger_kind + 0x23C, movement_profile[8:12])
    byte(uc, passenger_kind + 0x248,
         int(float(passenger_fields.get("waterline", 0))))
    byte(uc, passenger_kind + 0x24A, 0)
    byte(uc, passenger_kind + 0x249, 6)
    put(uc, passenger + 0x12B,
        int(float(passenger_fields.get("maxvelocity", 0)) * 65536))
    put(uc, passenger + 0x78, passenger_footprint)

    ground_nav, ground_mover = phase._alloc(0x180), phase._alloc(0x80)
    put(uc, ground_nav, 0x5F2A24, 0, passenger)
    put(uc, ground_mover, ground_nav, phase.GRID)
    put(uc, ground_mover + 0x20, 0)
    put(uc, ground_mover + 0x30, 0x7FFFFFFF)
    put(uc, ground_mover + 0x14, 0)
    uc.mem_write(ground_mover + 0x36, struct.pack("<H", 1))
    put(uc, passenger + 8, ground_mover)
    px, pz = TARGET
    py = heights[pz * width + px]
    put(uc, passenger + 0x68, px * SCALE * 65536, py * 65536,
        pz * SCALE * 65536)
    uc.mem_write(passenger + 0x74,
                 struct.pack("<hh", px - fx // 2, pz - fz // 2))

    # Retail uses +0xc0 as the ground unit's mount/pose anchor table during
    # movement. This bounded zero table avoids a null read outside the sim path.
    anchor = phase._alloc(0x400)
    uc.mem_write(anchor, bytes(0x400))
    put(uc, passenger + 0xC0, anchor)

    # Start both native bodies in the exact map sectors the real mover expects.
    put(uc, carrier + 0xA4, 0)
    put(uc, passenger + 0xA4, 0)
    for unit, cell in ((carrier, START), (passenger, TARGET)):
        sector = sectors + ((cell[1] // 8) * stride + cell[0] // 8) * 10
        _, error = phase.icd.call(0x506650, (unit, sector))
        if error:
            raise RuntimeError(("native body-sector insertion", cell, error))
        if struct.unpack("<I", uc.mem_read(unit + 0xA4, 4))[0] != sector:
            raise AssertionError(("body-sector link was not installed", cell))
    return (flight_nav, flight_mover, ground_nav, ground_mover,
            passenger_footprint, anchor)


def install_live_ground_search(phase, state, passenger, nav, mover,
                               grades, profile, fx, fz, width, height):
    uc, icd, live = phase.uc, phase.icd, state["live"]
    get = live.get
    owner = live.owner
    start = (TARGET[0] - fx // 2, TARGET[1] - fz // 2)
    controller = phase._alloc(0x20)
    phase.attach_live_request(passenger, mover, nav, controller,
                              start, (fx, fz))
    put(uc, nav + 4, 0)
    if get(nav + 4) != 0:
        raise AssertionError("Araarch navigator should begin without a controller")

    put(uc, GS + 0x19F30, 1)
    put(uc, GS + 0x174C8, 101)
    put(uc, GS + 0x174CC, 102)
    put(uc, GS + 0x19E88, width * SCALE)
    put(uc, GS + 0x19E8C, height * SCALE)
    visibility = get(GS + 0x19EF4)
    uc.mem_write(visibility, struct.pack("<" + "H" * (width * height // 4),
                                         *([0xFFFF] * (width * height // 4))))
    put(uc, owner + 0x8C, width // 2)
    put(uc, owner + 0x90, height // 2)
    byte(uc, GS + 0x306F, 0)
    put(uc, GS + 0x19EF8, state["sea"])

    # Make the native singleton scheduler enumerate only Araarch's live path
    # request; its passenger/carrier units remain in the normal entity pool.
    put(uc, owner + 0x74, passenger)
    put(uc, owner + 0x78, passenger)
    put(uc, OBJ + 0x115, passenger)
    put(uc, OBJ + 0x225, 12000)
    put(uc, OBJ + 0x165, 10_000_000)
    put(uc, OBJ + 0x5C, 1)
    uc.mem_write(0x634674, bytes(40))

    search_service = phase._alloc(0x22B)
    _, error = icd.call(0x415F80, (), ecx=search_service)
    if error:
        raise RuntimeError(("native path search service constructor", error))
    put(uc, GS + 0x19E70, search_service)

    worker_events, route_installs = [], []
    native_requests = []
    def route_alloc(_uc, stack):
        size = struct.unpack("<I", _uc.mem_read(stack, 4))[0]
        if size == 0x14 and route_alloc.controller:
            value, route_alloc.controller = route_alloc.controller, 0
            return 0, value
        return 0, phase._alloc(size)
    route_alloc.controller = controller
    def prepare(_uc, _stack):
        worker_events.append("prepare")
        return 2, 0
    def finish(_uc, _stack):
        worker_events.append("finish")
        return 1, 0
    def request_weight(_uc, stack):
        address = struct.unpack("<I", _uc.mem_read(stack, 4))[0]
        put(uc, address, 196608)
        return 1, address
    def visible(_uc, _stack):
        return 4, 1
    def no_gate(_uc, _stack):
        return 0, 0
    def no_special_body(_uc, _stack):
        return 2, 0
    def live_grade(_uc, stack):
        _who, world_x, _world_y, world_z = struct.unpack(
            "<Iiii", uc.mem_read(stack, 16))
        x = ((world_x >> 19) - fx) // 2
        z = ((world_z >> 19) - fz) // 2
        return 4, grades[z * width + x] if 0 <= x < width and 0 <= z < height else 0

    hooks = dict(icd.hooks)
    hooks.pop(0x4D4BF0, None)
    hooks.pop(0x4D6AD0, None)
    hooks.pop(0x4E4F50, None)
    hooks.pop(0x4E2470, None)
    lookup_method = get(get(nav) + 0x18)
    hooks.pop(lookup_method, None)
    hooks.update({
        0x4EB9E0: route_alloc,
        0x4E1EE0: prepare,
        0x4E2060: finish,
        0x413C80: visible,
        0x409FE0: no_gate,
        0x4DB640: live_grade,
        0x50E600: no_special_body,
        0x4161B0: request_weight,
    })
    icd.hooks = hooks

    def observe(machine, address, _size, _data):
        if address == 0x4E4F05 and machine.reg_read(UC_X86_REG_ESI) == nav:
            count = get(nav + 0x10C)
            words = struct.unpack("<" + "h" * (count * 2),
                                  machine.mem_read(nav + 12, count * 4)) if count else ()
            route_installs.append(list(zip(words[::2], words[1::2])))
        elif address == 0x4E4F50:
            sp = machine.reg_read(UC_X86_REG_ESP)
            ret, arg = struct.unpack("<II", machine.mem_read(sp, 8))
            native_requests.append((get(GS + 0x19F44), arg, ret))
    uc.hook_add(UC_HOOK_CODE, observe)

    return controller, worker_events, route_installs, native_requests


def run(args):
    root, hpitool = Path(args.retail_root).resolve(), Path(args.hpitool).resolve()
    # This companion setup only needs run_route's entity/order fixtures; the
    # Araarch route below overwrites its grade plane with a true full-map
    # GROUND2 plane. Keep the synthetic Vertrans setup on its proven corridor.
    state = run_route(root, hpitool, full_map=False, native_attachment=True)
    phase, live = state["phase"], state["live"]
    uc = phase.uc
    pool, carrier, passenger = install_native_entity_array(state)
    state["native_attachment"] = True
    map_data, sectors, stride = make_map(phase, root, hpitool)
    width, height = state["width"], state["height"]
    if (map_data[0], map_data[1], map_data[2]) != (width, height, state["sea"]):
        raise AssertionError("map-backed native height setup changed the route map")

    carrier_name, passenger_name = args.carrier.lower(), args.passenger.lower()
    carrier_fields = carrier_profile(hpitool, root, carrier_name, args.crusades)
    passenger_fields, movement, fx, fz = grade2_profile(
        hpitool, root, passenger_name)
    grades, grade_cache = ground_grade_plane(map_data, movement)
    phase.set_grade_plane(grades)
    uc.mem_write(phase.GRID + 4, movement[:4])
    uc.mem_write(phase.GRID + 8, movement[4:])

    # Empty retail COB VM descriptors keep all simulation callbacks native and
    # make only the absent presentation call-ins return their normal not-found.
    for unit in (carrier, passenger):
        vm, descriptor = phase._alloc(0x40), phase._alloc(0x40)
        put(uc, unit + 0xBC, vm)
        put(uc, vm + 0x0C, descriptor)
        put(uc, descriptor + 4, 0)

    owner = live.owner
    live.put(owner, 1)
    live.byte(owner + 0xEA, 1)
    live.put(owner + 0x74, carrier)
    live.put(owner + 0x78, passenger + 0x138)
    live.put(GS + 0x14E84, pool)
    live.put(GS + 0x14E88, passenger)
    live.put(GS + 0x19F30, 1)
    live.put(GS + 0x174C8, 101)
    live.put(GS + 0x174CC, 102)

    # Rebuild actual mission descriptors, then replace the previous synthetic
    # surface GROUND_PICKUP fixture pair with retail air/ground orders.
    goal_cell = START
    goal_x, goal_z = goal_cell
    goal_y = map_data[3][goal_z * width + goal_x]
    move_goal = (goal_x * SCALE * 65536, goal_y * SCALE * 65536,
                 goal_z * SCALE * 65536)
    carrier_order, passenger_order, seek_order = register_orders(
        phase, carrier, passenger, carrier_name, passenger_name, move_goal)

    # The carrier's type must follow selected balance FBI, while Araarch uses
    # its shipped GROUND2 MOVEINFO footprint/limits.
    set_units = set_unit_types_and_movers(
        phase, live, map_data, sectors, stride, carrier_fields,
        passenger_fields, movement, fx, fz)
    flight_nav, flight_mover, ground_nav, ground_mover, footprint, anchor = set_units
    passenger_controller, worker_events, route_installs, native_requests = \
        install_live_ground_search(phase, state, passenger, ground_nav,
                                   ground_mover, grades, movement,
                                   fx, fz, width, height)
    # Restore the ground-unit sector link after the worker setup, which uses its
    # +0xa4 slot for the real map sector table.
    sector_stride = stride
    passenger_sector = sectors + ((TARGET[1] // 8) * sector_stride +
                                  TARGET[0] // 8) * 10
    carrier_sector = sectors + ((START[1] // 8) * sector_stride +
                                START[0] // 8) * 10
    if live.get(passenger + 0xA4) != passenger_sector or \
            live.get(carrier + 0xA4) != carrier_sector:
        raise AssertionError(("initial native body-sector links",
                              hex(live.get(passenger + 0xA4)),
                              hex(passenger_sector),
                              hex(live.get(carrier + 0xA4)),
                              hex(carrier_sector)))

    # Native Move_Ground starts an ordinary point route. Let it install and
    # enqueue that GROUND2 route first, then initialize the carrier pickup on
    # the following monotonic tick so the air pursuit sees a live mover target.
    icd = phase.icd
    player_index = uc.mem_read(owner + 0xEB, 1)[0]
    queue_counter = 0x634674 + 4 * player_index
    first_tick = max(state["delivered_at"] + 1, live.get(GS + 0x19F44) + 1)
    live.put(GS + 0x19F44, first_tick)
    live.put(0x64186C, first_tick)
    _, error = icd.call(0x4D4DA0, (passenger_order + 0x22, 4),
                        ecx=passenger_order)
    if error:
        raise RuntimeError(("native Move_Ground controller initialization",
                            first_tick, error))
    if not live.get(ground_nav + 4) or not live.get(queue_counter):
        raise AssertionError(("Move_Ground did not install/enqueue its native GROUND2 route",
                              {"passenger_controller": hex(live.get(ground_nav + 4)),
                               "order_controller": hex(live.get(passenger_order + 0x6E)),
                               "passenger_stage": uc.mem_read(passenger_order + 5, 1)[0],
                               "queue": live.get(queue_counter),
                               "mission_position": struct.unpack(
                                   "<3i", uc.mem_read(passenger_order + 0x22, 12))}))
    route_worker_tick = None
    dispatch_trace = []

    def observe_initial_dispatch(machine, address, _size, _data):
        if address in (0x4D8450, 0x41A680, 0x4E4F50):
            dispatch_trace.append((live.get(GS + 0x19F44), address,
                                  machine.reg_read(UC_X86_REG_ECX),
                                  live.get(passenger_order + 0x6E),
                                  live.get(ground_nav + 4),
                                  live.get(queue_counter)))

    for address in (0x4D8450, 0x41A680, 0x4E4F50):
        uc.hook_add(UC_HOOK_CODE, observe_initial_dispatch,
                    begin=address, end=address)

    live.put(0x64186C, first_tick)
    if not live.get(ground_nav + 4) or not live.get(queue_counter):
        raise AssertionError(("Move_Ground lost its controller before path worker delivery",
                              dispatch_trace,
                              {"passenger_controller": hex(live.get(ground_nav + 4)),
                               "order_controller": hex(live.get(passenger_order + 0x6E)),
                               "passenger_stage": uc.mem_read(passenger_order + 5, 1)[0],
                               "queue": live.get(queue_counter)}))

    # Let the native path worker finish the already-admitted Move_Ground
    # request before VTOL_Pickup's next-tick dispatcher runs. Its route-cancel
    # calls share this headless fixture's singleton search scheduler.
    for tick in range(first_tick, first_tick + 501):
        live.put(GS + 0x19F44, tick)
        live.put(0x64186C, tick)
        if live.get(queue_counter) > 0:
            _, error = icd.call(0x416430, (1,), ecx=OBJ)
            if error:
                raise RuntimeError(("native ground path worker", tick, error))
        if route_installs and live.get(queue_counter) == 0 and \
                live.get(ground_nav + 4):
            route_worker_tick = tick
            break
    if route_worker_tick is None:
        raise AssertionError(("native pickup controllers/ground route were not delivered",
                              worker_events, native_requests,
                              live.get(ground_nav + 0x114),
                              live.get(queue_counter),
                              {"carrier_controller": hex(live.get(flight_nav + 4)),
                               "passenger_controller": hex(live.get(ground_nav + 4)),
                               "carrier_stage": uc.mem_read(carrier_order + 5, 1)[0],
                               "passenger_stage": uc.mem_read(passenger_order + 5, 1)[0],
                               "dispatch_trace": dispatch_trace}))
    installed_route = route_installs[-1]
    if len(installed_route) < 2:
        raise AssertionError(("native GROUND2 route is unexpectedly short",
                              installed_route))

    carrier_tick = route_worker_tick + 1

    # Keep the native global pathworker context live, then move carrier first
    # and Araarch second, matching entity-ID update order. Each unit gets its
    # real dispatcher, mover, and native position commit on every update.
    live.put(owner + 0x74, passenger)
    live.put(owner + 0x78, passenger)
    last_passenger = struct.unpack("<3i", uc.mem_read(passenger + 0x68, 12))
    moved_distance = 0.0
    carrier_goals = []
    nav_pops = []
    def observe_pop(machine, address, _size, _data):
        if address == 0x4E50A0 and machine.reg_read(UC_X86_REG_ESI) == ground_nav:
            nav_pops.append((live.get(GS + 0x19F44),
                             live.get(ground_nav + 0x10C),
                             live.get(passenger + 0x74)))
    uc.hook_add(UC_HOOK_CODE, observe_pop, begin=0x4E50A0, end=0x4E50A0)

    attachment_tick = None
    carrier_retired_tick = None
    move_completed_tick = None
    seek_retired_tick = None
    seek_route_tick = None
    post_move_wait_dispatches = 0
    seek_dispatch_states = []
    initial_route_count = len(route_installs)
    max_ticks = min(max(args.max_ticks, 1), 12000)
    dispatcher_returns = []
    def observe_dispatcher_return(machine, address, _size, _data):
        if address == 0x4D8501 and machine.reg_read(UC_X86_REG_EBX) == passenger:
            order = machine.reg_read(UC_X86_REG_ESI)
            phase_name = "active"
        elif address == 0x4D8622 and machine.reg_read(UC_X86_REG_EDI) == passenger:
            order = machine.reg_read(UC_X86_REG_ESI)
            phase_name = "queued"
        else:
            return
        dispatcher_returns.append((live.get(GS + 0x19F44), order,
                                   uc.mem_read(order + 4, 1)[0],
                                   machine.reg_read(UC_X86_REG_EAX),
                                   uc.mem_read(order + 5, 1)[0], phase_name))
    for address in (0x4D8501, 0x4D8622):
        uc.hook_add(UC_HOOK_CODE, observe_dispatcher_return,
                    begin=address, end=address)

    for tick in range(route_worker_tick + 1, route_worker_tick + max_ticks + 1):
        live.put(GS + 0x19F44, tick)
        live.put(0x64186C, tick)
        # Keep the originally constructed VTOL_Pickup order under its real
        # dispatcher until retail retires it. After that, advance only Araarch;
        # do not synthesize or reissue a carrier order when code 30 dispatches.
        if live.get(carrier + 0x60):
            _, error = icd.call(0x4D8450, (carrier,))
            if error:
                raise RuntimeError(("native VTOL_Pickup dispatcher", tick, error))
            if live.get(carrier + 0x60) == 0 and carrier_retired_tick is None:
                carrier_retired_tick = tick

        passenger_head_before = live.get(passenger + 0x60)
        _, error = icd.call(0x4D8450, (passenger,))
        if error:
            raise RuntimeError(("native Araarch passenger dispatcher", tick, error))
        passenger_head_after = live.get(passenger + 0x60)
        if passenger_head_before == passenger_order and \
                passenger_head_after != passenger_order and move_completed_tick is None:
            move_completed_tick = tick

        # This is the second half of retail's per-unit update: 0x51e1e5 calls
        # 0x4d8450 for the active chain, then 0x51e1eb calls 0x4d85e0 for the
        # queued chain. Calling only 0x4d8450 leaves code 30 entirely untested.
        _, error = icd.call(0x4D85E0, (passenger,))
        if error:
            raise RuntimeError(("native Araarch queued-order dispatcher", tick, error))
        code30_tick_returns = [row for row in dispatcher_returns
                               if row[0] == tick and row[2] == 30 and
                               row[5] == "queued"]
        if code30_tick_returns:
            seek_dispatch_states.append({
                "tick": tick,
                "returns": code30_tick_returns,
                "head": live.get(passenger + 0x60),
                "queued": live.get(passenger + 0x64),
                "stage": uc.mem_read(seek_order + 5, 1)[0],
                "wait_mask": struct.unpack("<I", uc.mem_read(seek_order + 6, 4))[0],
                "deadline": live.get(seek_order + 0x0A),
                "controller": live.get(seek_order + 0x6E),
                "navigator_controller": live.get(ground_nav + 4),
                "path_requests": live.get(queue_counter),
                "route_installs": len(route_installs),
                "cargo": (live.get(carrier + 0xAC) == passenger and
                          live.get(passenger + 0xA8) == carrier),
            })

        if live.get(queue_counter) > 0:
            _, error = icd.call(0x416430, (1,), ecx=OBJ)
            if error:
                raise RuntimeError(("native route worker", tick, error))

        if live.get(carrier + 0x60):
            _, error = icd.call(0x4DC800, (carrier,), ecx=flight_mover)
            if error:
                raise RuntimeError(("native ZONROC flight mover", tick, error))
            _, error = icd.call(0x51B2A0, (carrier,), ecx=flight_mover)
            if error:
                raise RuntimeError(("native ZONROC position commit", tick, error))
        _, error = icd.call(0x4DC800, (passenger,), ecx=ground_mover)
        if error:
            raise RuntimeError(("native Araarch GROUND2 mover", tick, error))
        _, error = icd.call(0x51B2A0, (passenger,), ecx=ground_mover)
        if error:
            raise RuntimeError(("native Araarch position commit", tick, error))

        carrier_controller = live.get(flight_nav + 4)
        if carrier_controller:
            goal = struct.unpack("<3i", uc.mem_read(carrier_controller + 0x26, 12))
            carrier_goals.append((tick, goal))

        pos = struct.unpack("<3i", uc.mem_read(passenger + 0x68, 12))
        dx = (pos[0] - last_passenger[0]) / 65536
        dz = (pos[2] - last_passenger[2]) / 65536
        moved_distance += (dx * dx + dz * dz) ** 0.5
        last_passenger = pos
        if live.get(carrier + 0xAC) == passenger and \
                live.get(passenger + 0xA8) == carrier:
            attachment_tick = tick
            break
        queued_after = live.get(passenger + 0x64)
        if queued_after != seek_order and live.get(passenger + 0x60) != seek_order:
            if seek_retired_tick is None:
                seek_retired_tick = tick
            # Stop at the real retirement result. Later allocator reuse can
            # make the freed mission's controller fields look nonzero.
            break
        if len(route_installs) > initial_route_count or \
                live.get(seek_order + 0x6E) != 0:
            if seek_route_tick is None:
                seek_route_tick = tick
            # The native worker above has now had its tick to install the
            # route. A controller without an install is still a real route
            # attempt (for example, a native failure result).
            break
        if move_completed_tick is not None and code30_tick_returns and \
                seek_retired_tick is None and seek_route_tick is None:
            post_move_wait_dispatches += 1
            if post_move_wait_dispatches >= 10:
                break

    if attachment_tick is None:
        active_head = live.get(passenger + 0x60)
        queued_head = live.get(passenger + 0x64)
        if uc.mem_read(passenger_order + 4, 1)[0] != 27 or \
                uc.mem_read(seek_order + 4, 1)[0] != 30:
            raise AssertionError(("native queued order codes changed",
                                  uc.mem_read(passenger_order + 4, 1)[0],
                                  uc.mem_read(seek_order + 4, 1)[0]))
        code27_returns = [row for row in dispatcher_returns if row[2] == 27]
        code30_returns = [row for row in dispatcher_returns
                          if row[2] == 30 and row[5] == "queued"]
        if move_completed_tick is None and seek_retired_tick is None and \
                seek_route_tick is None:
            raise AssertionError(("native Move_Ground did not complete before bounded trace ended",
                                  carrier_retired_tick, move_completed_tick,
                                  moved_distance, code27_returns[-4:],
                                  code30_returns[-4:], hex(active_head),
                                  hex(queued_head)))
        if move_completed_tick is not None and \
                (moved_distance < 100 or not code27_returns):
            raise AssertionError(("native Move_Ground did not complete physical travel",
                                  moved_distance, code27_returns[-4:], nav_pops[-8:]))
        move_result = (f"Move_Ground code 27 completed at tick {move_completed_tick}"
                       if move_completed_tick is not None else
                       f"Move_Ground code 27 was still active at tick {live.get(GS + 0x19F44)}")
        print(f"PASS: native {move_result}; "
              f"Araarch traveled {moved_distance:.1f}px; original VTOL_Pickup "
              f"retired at tick {carrier_retired_tick}; no boarding.")
        if seek_retired_tick is not None:
            outcome = f"code 30 retired from the queued list at tick {seek_retired_tick}"
        elif seek_route_tick is not None:
            outcome = f"code 30 began route/controller work at tick {seek_route_tick}"
        elif post_move_wait_dispatches:
            outcome = (f"code 30 remained queued for {post_move_wait_dispatches} "
                       "post-completion dispatches without route or attachment")
        else:
            outcome = "no code-30 wait/route/retire outcome was observed"
        print(f"  Queued dispatcher result: {outcome}; active={hex(active_head)}, "
              f"queued={hex(queued_head)}; route={installed_route}; "
              f"waypoint pops={len(nav_pops)}.")
        if seek_dispatch_states:
            print(f"  First code-30 queued dispatch: {seek_dispatch_states[0]}")
            if len(seek_dispatch_states) > 1:
                print(f"  Last code-30 queued dispatch: {seek_dispatch_states[-1]}")
        print(f"  Last handler returns: Move_Ground={code27_returns[-4:]}, "
              f"Move_Seek_Pickup={code30_returns[-4:]}")
        print("  This secondary-list trace does not compare ordinary primary-list Shift-load behavior.")
        return
    if not nav_pops:
        raise AssertionError("native GROUND2 mover never popped a route waypoint")
    if moved_distance < 100:
        raise AssertionError(("Araarch did not travel on native GROUND2 mover",
                              moved_distance, nav_pops[:4]))
    if live.get(carrier + 0x60) != 0:
        raise AssertionError(("carrier VTOL_Pickup order was not retired",
                              hex(live.get(carrier + 0x60))))
    carried = live.get(passenger + 0x60)
    if not carried or uc.mem_read(carried + 4, 1)[0] != 11:
        raise AssertionError(("native BeCarried order not installed", hex(carried)))
    if not carrier_goals or carrier_goals[-1][1][0] == 0:
        raise AssertionError(("native flight pursuit goals were not captured",
                              carrier_goals[-3:]))

    final_carrier = struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))
    final_passenger = struct.unpack("<3i", uc.mem_read(passenger + 0x68, 12))
    final_gap = ((final_carrier[0] - final_passenger[0]) ** 2 +
                 (final_carrier[2] - final_passenger[2]) ** 2) ** 0.5 / 65536
    print(f"PASS: Lake Lokken {carrier_name}/{passenger_name} native GROUND2 route "
          f"and VTOL pickup boarded Araarch at tick {attachment_tick}; "
          f"carrier order retired and BeCarried code 11 installed.")
    print(f"  Native 0x508cd0 grades cached: {grade_cache}; route worker tick "
          f"{route_worker_tick}, installed waypoints={len(installed_route)}, "
          f"waypoint pops={len(nav_pops)}, Araarch travel={moved_distance:.1f}px.")
    print(f"  Final carrier/Araarch positions: "
          f"({final_carrier[0]/65536:.1f},{final_carrier[2]/65536:.1f}) / "
          f"({final_passenger[0]/65536:.1f},{final_passenger[2]/65536:.1f}), "
          f"planar gap={final_gap:.1f}px; sector grid={stride}x{stride}; "
          f"Araarch footprint={fx}x{fz}; reciprocal cargo links verified.")
    print(f"  Retail executes both dispatchers, native route search and physical "
          f"movers, live sector relinks, passenger release/attachment and order "
          f"transition. Controlled boundaries: worker scheduling/UI/effects, "
          f"visibility, feature-definition bodies, and empty COB method tables. "
          f"The test ends at attachment, before display/mount animation update.")


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo / "build-o2/hpitool"))
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    parser.add_argument("--passenger", default=DEFAULT_PASSENGER)
    parser.add_argument("--max-ticks", type=int, default=9000)
    parser.add_argument("--crusades", action="store_true")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
