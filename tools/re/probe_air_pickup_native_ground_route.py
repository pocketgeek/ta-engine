#!/usr/bin/env python3
"""Join Araarch's real Lake Lokken ground route to ZONROC's native pickup.

This composes the existing full-map native pickup entity fixture with the
GROUND2 passenger route/mover setup. Retail builds the shipped TNT height
sectors, searches GROUND2 grades from native 0x508cd0, dispatches the actual
Move_Seek_Pickup and VTOL_Pickup orders, runs the native path worker and both
native movers, and boards Araarch when the two live units meet. The emulator
keeps the established headless boundaries for worker scheduling services,
visibility, feature definitions, effects/UI, and the empty COB method table.
The passenger ground movement, flight pursuit, cargo attachment, and order
transition execute in KINGDOMS.icd. No retail GUI is launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_ground_route.py
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
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESI, UC_X86_REG_ESP


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


def register_orders(phase, carrier, passenger, carrier_name, passenger_name):
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
    move = lookup(0x604CF8)
    if vtol != (62, 0x41A680, b"vtol_pickup"):
        raise AssertionError(("retail VTOL_Pickup registry", vtol))
    if move != (30, 0x403430, b"move_seek_pickup"):
        raise AssertionError(("retail Move_Seek_Pickup registry", move))

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

    carrier_order, passenger_order = phase._alloc(0x100), phase._alloc(0x100)
    for code, target, order, unit in (
            (30, carrier, passenger_order, passenger),
            (62, passenger, carrier_order, carrier)):
        result, error = icd.call(
            0x4D6C40, (code, target, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=order)
        if error or result != order:
            raise RuntimeError(("native transport-order constructor", code, result, error))
        _, error = icd.call(0x4D7750, (unit, order))
        if error:
            raise RuntimeError(("native transport-order insertion", code, error))
    if (get(carrier + 0x60), get(passenger + 0x60)) != \
            (carrier_order, passenger_order):
        raise AssertionError("native pickup order heads were not installed")
    if get(carrier + 0xC4) != passenger_order + 0x12 or \
            get(passenger + 0xC4) != carrier_order + 0x12:
        raise AssertionError("native pickup orders lack reciprocal target refs")
    return carrier_order, passenger_order


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
    py = sea
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
    carrier_order, passenger_order = register_orders(
        phase, carrier, passenger, carrier_name, passenger_name)

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

    # Native's code-30 dispatcher creates Araarch's real search circle and
    # enqueues a full-map route. Let that complete first, then initialize the
    # carrier pickup on the following monotonic tick, as in the proven paired
    # surface fixture.
    icd = phase.icd
    player_index = uc.mem_read(owner + 0xEB, 1)[0]
    queue_counter = 0x634674 + 4 * player_index
    first_tick = max(state["delivered_at"] + 1, live.get(GS + 0x19F44) + 1)
    route_worker_tick = None
    dispatch_trace = []

    def observe_initial_dispatch(machine, address, _size, _data):
        if address in (0x4D8450, 0x403430, 0x41A680, 0x4E4F50):
            dispatch_trace.append((live.get(GS + 0x19F44), address,
                                  machine.reg_read(UC_X86_REG_ECX),
                                  live.get(passenger_order + 0x6E),
                                  live.get(ground_nav + 4),
                                  live.get(queue_counter)))

    for address in (0x4D8450, 0x403430, 0x41A680, 0x4E4F50):
        uc.hook_add(UC_HOOK_CODE, observe_initial_dispatch,
                    begin=address, end=address)

    live.put(GS + 0x19F44, first_tick)
    live.put(0x64186C, first_tick)
    _, error = icd.call(0x4D8450, (passenger,))
    if error:
        raise RuntimeError(("native Move_Seek_Pickup initial dispatch",
                            first_tick, error))
    live.put(0x64186C, first_tick)
    if not live.get(ground_nav + 4) or not live.get(queue_counter):
        raise AssertionError(("Araarch did not install/enqueue its native GROUND2 route",
                              dispatch_trace,
                              {"passenger_controller": hex(live.get(ground_nav + 4)),
                               "order_controller": hex(live.get(passenger_order + 0x6E)),
                               "passenger_stage": uc.mem_read(passenger_order + 5, 1)[0],
                               "queue": live.get(queue_counter),
                               "passenger_sector": hex(live.get(passenger + 0xA4)),
                               "carrier_sector": hex(live.get(carrier + 0xA4))}))

    carrier_tick = first_tick + 1
    live.put(GS + 0x19F44, carrier_tick)
    live.put(0x64186C, carrier_tick)
    _, error = icd.call(0x4D8450, (carrier,))
    if error:
        raise RuntimeError(("native VTOL_Pickup initial dispatch", carrier_tick, error))
    live.put(0x64186C, carrier_tick)

    for tick in range(carrier_tick, carrier_tick + 501):
        live.put(GS + 0x19F44, tick)
        live.put(0x64186C, tick)
        # Dispatcher starts already delivered its initial order at this tick;
        # subsequent dispatches maintain its controller and live target.
        if tick > carrier_tick:
            for unit, label in ((passenger, "Move_Seek_Pickup"),
                                (carrier, "VTOL_Pickup")):
                _, error = icd.call(0x4D8450, (unit,))
                if error:
                    raise RuntimeError((f"native {label} initial dispatch", tick, error))
        live.put(0x64186C, tick)
        if live.get(queue_counter) > 0:
            _, error = icd.call(0x416430, (1,), ecx=OBJ)
            if error:
                raise RuntimeError(("native ground path worker", tick, error))
        if route_installs and live.get(queue_counter) == 0 and \
                live.get(ground_nav + 4) and live.get(flight_nav + 4):
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
    max_ticks = min(max(args.max_ticks, 1), 12000)
    for tick in range(route_worker_tick + 1, route_worker_tick + max_ticks + 1):
        live.put(GS + 0x19F44, tick)
        live.put(0x64186C, tick)
        # Retail's two dispatchers share the global entity clock. The air
        # mission samples Araarch's current position before either mover step.
        for unit, label in ((carrier, "VTOL_Pickup"),
                            (passenger, "Move_Seek_Pickup")):
            _, error = icd.call(0x4D8450, (unit,))
            if error:
                raise RuntimeError((f"native {label} dispatcher", tick, error))

        if live.get(queue_counter) > 0:
            _, error = icd.call(0x416430, (1,), ecx=OBJ)
            if error:
                raise RuntimeError(("native route worker", tick, error))

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
        if live.get(carrier + 0x60) == 0:
            raise AssertionError(("VTOL_Pickup retired before attachment", tick))

    if attachment_tick is None:
        raise AssertionError(("native moving Araarch was not boarded",
                              max_ticks, "passenger", last_passenger,
                              "carrier", struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12)),
                              "route", installed_route,
                              "pops", nav_pops[-8:]))
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
