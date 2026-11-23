#!/usr/bin/env python3
"""Replay one native GROUND_PICKUP route request over a Lake Lokken corridor.

This probe joins the real surface-pickup dispatcher/controller to the native
0x416430 route worker. Only a narrow corridor of map cells receives grades
from retail 0x508cd0 using Lake Lokken TNT terrain; all cells outside it are
blocked. It ends when the route is delivered. It does not run the boat mover,
arrival callback, passenger transfer, or boarding.

Run from the repository root:
    python3 tools/re/probe_surface_pickup_native_map_route.py
"""
import argparse
import importlib
import struct
from pathlib import Path

from balance_inputs import unit_properties
from check_surface_unload_map_grades import (
    native_grade_reader,
    native_water_profile,
)
from check_surface_unload_map_release import cat, parse_tnt
from emuphase import GS, OBJ, Phase
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESI


MAP = "Lake Lokken"
CARRIER = "vertrans"
PASSENGER = "araarch"
START = (240, 120)
TARGET = (240, 350)
WORLD_UNITS_PER_MAP_CELL = 16
CORRIDOR_X = (228, 253)  # half-open
CORRIDOR_Z = (110, 361)  # half-open

# Values observed in the matching Vertrans Lake Lokken World attempt profile.
# The dimensions, water limits/slopes, turn rate, and transport distance below
# are read from the shipped MOVEINFO/FBI assets; the route cost multipliers and
# heavy-floater flag mirror that captured attempt profile.
ROAD_MULTIPLIER_FIXED = 78643
WATER_MULTIPLIER_FIXED = 65536
HEAVY_FLOATER = True
MOVER_FLAGS = 0x1000
HALF_CELL_TICKS = 2
CAPTURED_ROUTE_WEIGHT = 196608
EXPECTED_ROUTE = [
    (3840, 1920),
    (4064, 2144),
    (4064, 3600),
    (3824, 3840),
    (4048, 4064),
    (3824, 4288),
    (4048, 4512),
    (3824, 4736),
    (4048, 4960),
    (3824, 5184),
    (3824, 5360),
]


def install_pickup_fixture_on_phase(phase, native_attachment=False):
    """Use the existing dispatcher fixture on Phase's ICD without freezing hooks."""
    fixture_module = importlib.import_module(
        "probe_transport_surface_pickup_callbacks")
    original_icd_factory = fixture_module.Icd
    original_freeze_hooks = phase.icd.freeze_hooks
    fixture_module.Icd = lambda: phase.icd
    phase.icd.freeze_hooks = lambda: None
    # Native map-backed movement asks these two game-side services. Match the
    # established native surface-mover fixture boundaries before Phase later
    # freezes its hook table for the route-worker replay.
    phase.icd.hooks[0x51AD20] = lambda _uc, _args: (1, 0)
    phase.icd.hooks[0x56C640] = lambda _uc, _args: (8, 0)
    try:
        pickup = fixture_module.SurfacePickup()
        if not native_attachment:
            # 0x51b4f0 resolves native unit IDs through the game's global
            # contiguous entity pool. The default route-only probe keeps
            # fixture entities in a synthetic heap, so model its established
            # attachment boundary. The full-map mission probe instead places
            # both entities in actual 0x138-byte entity-pool slots and lets the
            # retail attachment routine execute.
            def attach(uc, stack_args):
                import struct
                passenger, carrier, _sentinel, _flags, _mode = struct.unpack(
                    "<5I", uc.mem_read(stack_args, 20))
                assert (passenger, carrier) == (pickup.passenger, pickup.carrier), (
                    hex(passenger), hex(carrier))
                pickup.put(carrier + 0xAC, passenger)
                pickup.put(passenger + 0xA8, carrier)
                pickup.put(passenger + 0x60, 0)
                pickup.put(passenger + 0x130,
                           pickup.get(passenger + 0x130) | 0x02000000)
                for offset in (0x68, 0x6C, 0x70):
                    pickup.put(passenger + offset, pickup.get(carrier + offset))
                return 5, 0
            phase.icd.hooks[0x51B4F0] = attach
    finally:
        fixture_module.Icd = original_icd_factory
        phase.icd.freeze_hooks = original_freeze_hooks
    return pickup


def run(retail_root, hpitool, *, full_map=False, native_attachment=False):
    root = Path(retail_root)
    phase = Phase(480, 480)
    assert phase.construct() is None
    live = install_pickup_fixture_on_phase(phase, native_attachment)
    uc, icd = phase.uc, phase.icd
    put, get, byte = live.put, live.get, live.byte
    carrier, passenger, kind, mission = (
        live.carrier, live.passenger, live.kind, live.mission)
    game, mover, owner, nav = live.game, live.mover, live.owner, live.nav

    # The first native dispatch starts GROUND_PICKUP. Place the actual sea
    # carrier and land passenger before retail creates the target controller.
    first_dispatch = live.dispatch(1)
    assert first_dispatch["active"] and first_dispatch["stage"] == 1, first_dispatch

    tnt = cat(hpitool, root, "maps.hpi", f"Maps/{MAP}.tnt")
    map_data = parse_tnt(tnt)
    width, height, sea, heights, feature_ids, feature_count = map_data
    assert (width, height, sea) == (480, 480, 58), (width, height, sea)

    movement, packed_profile, foot_x, foot_z = native_water_profile(
        hpitool, str(root), CARRIER)
    assert movement == "water4" and (foot_x, foot_z) == (4, 4), (
        movement, foot_x, foot_z)
    (class_x, class_z, max_depth, min_depth, bad_max_depth, bad_min_depth,
     max_slope, bad_slope, max_water_slope, bad_water_slope) = struct.unpack(
        "<6h4B", packed_profile)
    assert (class_x, class_z) == (foot_x, foot_z)

    carrier_fbi = unit_properties(cat(
        hpitool, root, "data.hpi", f"units/{CARRIER}.fbi").decode("latin1"))
    transport_distance = int(float(carrier_fbi["transportdistance"]))
    turn_rate = int(float(carrier_fbi["turnrate"]))
    assert transport_distance == 300 and turn_rate == 150, carrier_fbi

    scale = WORLD_UNITS_PER_MAP_CELL
    put(carrier + 0x68, START[0] * scale * 65536)
    put(carrier + 0x6C, sea * scale * 65536)
    put(carrier + 0x70, START[1] * scale * 65536)
    put(carrier + 0x74, (START[0] - foot_x // 2) |
        ((START[1] - foot_z // 2) << 16))
    put(carrier + 0x78, foot_x | (foot_z << 16))
    put(carrier + 2, 1)
    put(passenger + 0x68, TARGET[0] * scale * 65536)
    put(passenger + 0x6C, sea * scale * 65536)
    put(passenger + 0x70, TARGET[1] * scale * 65536)
    put(passenger + 8, mover)
    put(mover + 0x20, 0)

    put(kind + 0x126, foot_x | (foot_z << 16))
    put(kind + 0x18E, turn_rate)
    put(kind + 0x172, ROAD_MULTIPLIER_FIXED)
    put(kind + 0x16E, WATER_MULTIPLIER_FIXED)
    uc.mem_write(kind + 0x192, struct.pack(
        "<4h", max_depth, min_depth, bad_max_depth, bad_min_depth))
    uc.mem_write(kind + 0x23C, bytes((max_slope, bad_slope,
                                     max_water_slope, bad_water_slope)))
    put(kind + 0x23E, transport_distance)
    put(kind + 0x260, 0x80000 if HEAVY_FLOATER else 0)
    put(kind + 0x264, 0x200)
    put(kind + 0x14A, 1 << 16)
    byte(kind + 0x24B, 0)
    byte(kind + 0x249, HALF_CELL_TICKS)
    put(mover + 0x36, MOVER_FLAGS)

    # Native map records: the same TNT height-corner and feature-id layout as
    # check_surface_unload_map_route.py. Feature definitions are intentionally
    # zero-filled; this focused route checks terrain grades, not feature bodies.
    records = bytearray(width * height * 14)
    for z in range(height):
        for x in range(width):
            i = z * width + x
            offset = i * 14
            corners = (
                heights[i],
                heights[z * width + min(x + 1, width - 1)],
                heights[min(z + 1, height - 1) * width + x],
                heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)],
            )
            records[offset + 4:offset + 7] = bytes(
                (heights[i], max(corners), min(corners)))
            struct.pack_into("<H", records, offset + 8, feature_ids[i])
    uc.mem_write(phase.cells_addr, bytes(records))
    put(GS + 0x19EF8, sea)
    feature_table = phase._alloc(max(320, feature_count * 320))
    uc.mem_write(feature_table, bytes(max(320, feature_count * 320)))
    put(GS + 0x19EDC, feature_table)
    put(GS + 0x19EC0, feature_count)

    # GROUND_PICKUP installs the circle controller in stage 2. This fixture's
    # temporary allocator returns the controller address for all allocations;
    # restore Phase's arena allocator immediately after controller creation.
    queued = []
    pending = [False]
    worker_events = []
    deliveries = []

    def enqueue(_uc, stack_args):
        value = struct.unpack("<I", _uc.mem_read(stack_args, 4))[0]
        queued.append(value)
        pending[0] = True
        worker_events.append(("enqueue", value))
        return 1, 0

    icd.hooks[0x4E4F50] = enqueue
    second_dispatch = live.dispatch(2)
    controller = get(nav + 4)
    assert second_dispatch["active"] and controller, second_dispatch
    assert get(controller) == 0x5F28D8
    goal_cell = struct.unpack("<hh", uc.mem_read(controller + 8, 4))
    circle_radius = struct.unpack("<i", uc.mem_read(controller + 0x0C, 4))[0]
    assert goal_cell == (238, 348), goal_cell
    assert circle_radius == transport_distance - 16 == 284, circle_radius

    def route_alloc(_uc, stack_args):
        size = struct.unpack("<I", _uc.mem_read(stack_args, 4))[0]
        return 0, phase._alloc(size)

    icd.hooks[0x4EB9E0] = route_alloc
    phase.attach_live_request(
        carrier, mover, nav, controller,
        (START[0] - foot_x // 2, START[1] - foot_z // 2),
        (foot_x, foot_z))

    # Cache native grades in the narrow route corridor by default; the optional
    # full-map continuation grades the entire shipped TNT plane.
    put(0x62D55C, GS)
    uc.mem_write(phase.GRID + 8, packed_profile[4:])
    native_grade = native_grade_reader(map_data, packed_profile)
    grades = [0] * (width * height)
    grade_count = 0
    grade_x = (0, width) if full_map else CORRIDOR_X
    grade_z = (0, height) if full_map else CORRIDOR_Z
    for z in range(*grade_z):
        for x in range(*grade_x):
            grades[z * width + x] = native_grade(x, z)
            grade_count += 1
    phase.set_grade_plane(grades)
    assert all(grades[z * width + 240] in (4, 6)
               for z in range(123, 337)), "route corridor contains a blocked native grade"

    # Set up the same single-player worker lookup and queue state used by the
    # native unload-route worker probe.
    put(OBJ + 0x115, carrier - 0x138)
    put(OBJ + 0x225, 12000)
    put(GS + 0x19E70, OBJ)
    config = GS + 0x600000
    put(0x62D558, config)
    put(config, GS + 0x700000)
    put(config + 8, config + 0x100)
    put(config + 0x10C, 4)
    uc.mem_write(GS + 0x3068, b"\x01\x00")
    worker_owner = GS + 0x2404
    first_slot = carrier - 0x138
    first_page = first_slot & ~0xFFF
    uc.mem_map(first_page, 0x1000)
    uc.mem_write(first_slot, bytes(0x138))
    put(worker_owner, 1)
    uc.mem_write(worker_owner + 0xEA, b"\x01\x00")
    put(worker_owner + 0x74, first_slot)
    put(worker_owner + 0x78, first_slot + 3 * 0x138)
    put(OBJ + 0x165, 10_000_000)
    put(OBJ + 0x5C, 1)
    put(0x634674, 1)

    def lookup(_uc, _stack_args):
        worker_events.append(("lookup",))
        return 0, nav if pending[0] else 0

    def prepare(_uc, _stack_args):
        worker_events.append(("prepare",))
        return 2, 0

    def notify(_uc, _stack_args):
        worker_events.append(("notify",))
        return 1, 0

    def finish(_uc, _stack_args):
        worker_events.append(("finish",))
        pending[0] = False
        return 1, 0

    def request_weight(_uc, stack_args):
        address = struct.unpack("<I", _uc.mem_read(stack_args, 4))[0]
        put(address, CAPTURED_ROUTE_WEIGHT)
        return 1, address

    def visible(_uc, _stack_args):
        return 4, 1

    def no_gate(_uc, _stack_args):
        return 0, 0

    def no_special_body(_uc, _stack_args):
        return 2, 0

    def live_grade(_uc, stack_args):
        _who, world_x, _world_y, world_z = struct.unpack(
            "<Iiii", uc.mem_read(stack_args, 16))
        x = ((world_x >> 19) - foot_x) // 2
        z = ((world_z >> 19) - foot_z) // 2
        if not (0 <= x < width and 0 <= z < height):
            return 4, 0
        return 4, grades[z * width + x]

    nav_vtable = get(nav)
    lookup_method = get(nav_vtable + 0x18)
    icd.hooks.update({
        0x4E4F50: enqueue,
        0x4E1EE0: prepare,
        0x4E2470: notify,
        0x4E2060: finish,
        0x413C80: visible,
        0x409FE0: no_gate,
        0x4DB640: live_grade,
        0x50E600: no_special_body,
        0x4161B0: request_weight,
        lookup_method: lookup,
    })

    # Ask the retail navigator to queue its current pickup circle, then let the
    # original singleton route worker find, search, and install the route.
    _, error = icd.call(0x4E54E0, (controller,), ecx=nav)
    assert error is None, ("native pickup SetDestination", error)
    assert queued and pending[0], (queued, pending[0])

    def capture_delivery(uc0, _address, _size, _data):
        if uc0.reg_read(UC_X86_REG_ESI) != nav:
            return
        count = get(nav + 0x10C)
        words = struct.unpack("<" + "h" * (count * 2),
                              uc0.mem_read(nav + 12, count * 4)) if count else ()
        deliveries.append(list(zip(words[::2], words[1::2])))

    icd.uc.hook_add(UC_HOOK_CODE, capture_delivery,
                    begin=0x4E4F05, end=0x4E4F05)
    icd.freeze_hooks()

    delivered_at = None
    for tick in range(2, 502):
        put(GS + 0x19F44, tick)
        put(0x634674, int(pending[0]))
        _, error = icd.call(0x416430, (1,), ecx=OBJ)
        assert error is None, ("retail pickup route worker", tick, error)
        if deliveries and not pending[0]:
            delivered_at = tick
            break
    assert delivered_at is not None, {
        "queued": queued,
        "worker_events": worker_events[-30:],
        "phase": phase.phase(),
        "route_count": get(nav + 0x10C),
    }

    route = deliveries[0]
    if full_map:
        assert route == [
            (3840, 1920), (4064, 2144), (4064, 3440),
            (3840, 3664), (3840, 5360),
        ], route
    else:
        assert route == EXPECTED_ROUTE, route
    assert route[0] == (START[0] * scale, START[1] * scale), route[0]
    end_x, end_z = route[-1]
    dx = end_x - TARGET[0] * scale
    dz = end_z - TARGET[1] * scale
    assert dx * dx + dz * dz <= circle_radius * circle_radius, (
        route[-1], TARGET, circle_radius)

    # Retail subtracts 16px when constructing GROUND_PICKUP's circle. In this
    # fixture the type is set to 300px to match shipped Vertrans.

    print(
        f"PASS: Lake Lokken {CARRIER}/{PASSENGER} native GROUND_PICKUP "
        f"controller {hex(get(controller))}, goal cell {goal_cell}, "
        f"radius {circle_radius}; 0x4e54e0 queued through 0x4e4f50 and "
        f"0x416430 delivered {len(route)} waypoints on worker tick {delivered_at}."
    )
    print(f"  Route: {route}")
    if full_map:
        print(
            f"  Native 0x508cd0 grades cover all {grade_count} TNT cells "
            f"({width}x{height}); no off-map or off-corridor cells are blocked. "
            "Feature-definition bodies are zero-filled."
        )
    else:
        print(
            f"  Native 0x508cd0 grades cover {grade_count} TNT cells in "
            f"x={CORRIDOR_X[0]}..{CORRIDOR_X[1]-1}, "
            f"z={CORRIDOR_Z[0]}..{CORRIDOR_Z[1]-1}; cells outside the corridor "
            "are blocked. Feature-definition bodies are zero-filled."
        )
    print(
        f"  Shipped Vertrans transportdistance {transport_distance} produces "
        f"the native GROUND_PICKUP circle radius {transport_distance}-16="
        f"{circle_radius}. "
        + ("The full-map continuation uses native unit IDs and attachment."
           if full_map and native_attachment else
           "This route-phase checkpoint ends before physical mover/boarding; "
           "probe_surface_pickup_native_mission.py continues it.")
    )
    # Keep the route probe directly reusable by the map-backed mission probe.
    # These are live emulator objects, so callers must continue immediately and
    # must not serialize or reuse them after the Unicorn instance is discarded.
    return {
        "phase": phase, "live": live, "root": root, "hpitool": hpitool,
        "map_data": map_data, "packed_profile": packed_profile,
        "foot_x": foot_x, "foot_z": foot_z, "profile_limits": (
            max_depth, min_depth, bad_max_depth, bad_min_depth,
            max_slope, bad_slope, max_water_slope, bad_water_slope),
        "carrier_fbi": carrier_fbi, "transport_distance": transport_distance,
        "route": route, "controller": controller, "grades": grades,
        "grade_count": grade_count, "delivered_at": delivered_at,
        "feature_count": feature_count, "width": width, "height": height,
        "sea": sea,
    }


def main():
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo_root / "build/hpitool"))
    args = parser.parse_args()
    run(Path(args.retail_root).resolve(), Path(args.hpitool).resolve())


if __name__ == "__main__":
    main()
