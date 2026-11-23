#!/usr/bin/env python3
"""Pair Lake Lokken's World shore release with retail's headless unload path.

The existing map-route checker verifies the native route to the unload circle.
This fixture then delivers the real native route-arrival callback at the World
carrier's completed position and runs retail's GROUND_UNLOAD dispatcher,
0x507d10 placement test, cargo detach, and PARK transition. No game GUI is used.
"""
import argparse
import math
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

from emu import HEAP, Icd
from probe_transport_surface_unload_callbacks import SurfaceUnload


MAP = "Lake Lokken"
START = (240, 120)
TARGET = (240, 350)
CARRIER = "vertrans"
PASSENGER = "araarch"


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True)


def cat(hpitool, root, archive, internal_path):
    return subprocess.run([hpitool, "cat", str(root / archive), internal_path],
                          check=True, capture_output=True).stdout


def parse_tnt(data):
    u32 = lambda off: struct.unpack_from("<I", data, off)[0]
    if u32(0) != 0x4000:
        raise AssertionError("retail map is not a v0x4000 TNT")
    width, height, sea = u32(4), u32(8), u32(12)
    heights = data[u32(16):u32(16) + width * height]
    feature_offset = u32(20)
    features = [struct.unpack_from("<H", data, feature_offset + i * 2)[0]
                for i in range(width * height)]
    feature_count = u32(28)
    return width, height, sea, heights, features, feature_count


def movement_profile(text, unit_text):
    match = re.search(r"movementclass\s*=\s*([^;]+);", unit_text, re.I)
    if not match or match.group(1).strip().lower() != "ground2":
        raise AssertionError("Araarch no longer uses GROUND2")
    for body in re.findall(r"\[[^]]+\]\s*\{([^}]*)\}", text, re.S):
        fields = dict((key.lower(), value.strip()) for key, value in
                      re.findall(r"([a-z][a-z0-9_]*)\s*=\s*([^;]+);", body, re.I))
        if fields.get("name", "").lower() == "ground2":
            return (int(fields["footprintx"]), int(fields["footprintz"]),
                    int(fields["maxwaterdepth"]), int(fields["maxslope"]),
                    int(fields["maxwaterslope"]))
    raise AssertionError("GROUND2 was not found in retail MOVEINFO")


def native_placement_oracle(map_data, profile):
    width, height, sea, heights, features, feature_count = map_data
    icd = Icd()
    icd.uc.mem_map(0x73000000, 0x400000)
    game, cells = HEAP + 0x1000, 0x73000000
    feature_table = cells + width * height * 14 + 0x1000
    entities, kind = HEAP + 0x30000, HEAP + 0x40000
    put = lambda address, fmt, *values: icd.uc.mem_write(
        address, struct.pack(fmt, *values))
    put(0x62D55C, "<I", game)
    put(game + 0x19E98, "<II", width, height)
    put(game + 0x19F04, "<I", cells)
    put(game + 0x19EDC, "<I", feature_table)
    put(game + 0x19EC0, "<I", feature_count)
    put(game + 0x14E84, "<II", entities, entities + 312 * 16)
    icd.uc.mem_write(game + 0x19EF8, bytes((sea,)))

    records = bytearray(width * height * 14)
    for z in range(height):
        for x in range(width):
            index = z * width + x
            offset = index * 14
            sample = heights[index]
            corners = (heights[z * width + x],
                       heights[z * width + min(x + 1, width - 1)],
                       heights[min(z + 1, height - 1) * width + x],
                       heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)])
            records[offset + 4] = sample
            records[offset + 5] = max(corners)
            records[offset + 6] = min(corners)
            struct.pack_into("<H", records, offset + 8, features[index])
    icd.uc.mem_write(cells, bytes(records))
    icd.uc.mem_write(feature_table, bytes(feature_count * 320))
    icd.uc.mem_write(entities, bytes(312 * 16))
    icd.uc.mem_write(kind, bytes(0x400))
    foot_x, foot_z, max_water, max_slope, max_water_slope = profile
    put(kind + 0x126, "<hh", foot_x, foot_z)
    # World passes the full land interval to retailMobilePlacement for ground
    # passengers; water units instead use their positive min-water threshold.
    put(kind + 0x192, "<hh", max_water, -10000)
    icd.uc.mem_write(kind + 0x23C, bytes((max_slope, max_water_slope)))
    icd.uc.mem_write(kind + 0x24A, b"\x01")
    icd.freeze_hooks()

    blocker_id = 2
    blocker = entities + blocker_id * 312
    blocker_nav = HEAP + 0x50000
    icd.uc.mem_write(blocker_nav, bytes(0x180))
    put(blocker + 2, "<H", blocker_id)
    put(blocker + 0x08, "<I", blocker_nav)
    put(blocker + 0x0B4, "<I", kind)
    put(blocker + 0x130, "<I", 0x1000000)
    put(blocker + 0x78, "<hh", foot_x, foot_z)
    occupied = set()

    def set_blocker(x_raw=None, z_raw=None, moving=False, heading=0, speed_raw=0):
        """Update the native mobile-placement oracle's live cell occupancy."""
        nonlocal occupied
        for x, z in occupied:
            put(cells + (z * width + x) * 14, "<H", 0)
        occupied = set()
        if x_raw is None or z_raw is None:
            put(blocker_nav + 0x20, "<i", 0)
            return
        put(blocker_nav + 0x20, "<i", int(speed_raw) if moving else 0)
        put(blocker + 0x68, "<iii", int(x_raw), 0, int(z_raw))
        put(blocker + 0x7E, "<H", int(heading) & 0xFFFF)
        cell_x = (int(x_raw) - (foot_x - 1) * 8 * 65536) // (16 * 65536)
        cell_z = (int(z_raw) - (foot_z - 1) * 8 * 65536) // (16 * 65536)
        put(blocker + 0x74, "<hh", cell_x, cell_z)
        for z in range(max(0, cell_z), min(height, cell_z + foot_z)):
            for x in range(max(0, cell_x), min(width, cell_x + foot_x)):
                put(cells + (z * width + x) * 14, "<H", blocker_id)
                occupied.add((x, z))

    def check(args, _call):
        result, error = icd.call(0x507D10,
            (kind, args[1], args[2], args[3], args[4]))
        if error:
            raise RuntimeError(error)
        return result

    check.set_blocker = set_blocker
    return check


def footprint_origin(position, size):
    return math.floor((position - (size - 1) * 8) / 16)


def paired_check(binary, root, hpitool, crusades):
    balance = int(crusades)
    map_data = parse_tnt(cat(hpitool, root, "maps.hpi", "Maps/Lake Lokken.tnt"))
    width, height, sea, heights, features, feature_count = map_data
    passenger_profile = movement_profile(
        cat(hpitool, root, "data.hpi", "gamedata/moveinfo.tdf").decode("latin1"),
        cat(hpitool, root, "data.hpi", "units/araarch.fbi").decode("latin1"))
    assert (width, height, sea) == (480, 480, 58), (width, height, sea)
    foot_x, foot_z = passenger_profile[:2]
    landing_features = [features[z * width + x]
                        for z in range(TARGET[1], TARGET[1] + foot_z)
                        for x in range(TARGET[0], TARGET[0] + foot_x)]
    assert landing_features == [0xFFFF] * (foot_x * foot_z), landing_features

    route_env = {"TAK_DUMP_GRADE_PLANE": "1"}
    route = subprocess.run([binary, "--surface-unload-map-route-type", str(root), MAP,
        CARRIER, PASSENGER, str(START[0]), str(START[1]), str(TARGET[0]),
        str(TARGET[1]), str(balance)], check=True, capture_output=True, text=True,
        env={**os.environ, **route_env})
    profile = next(line for line in route.stderr.splitlines()
                   if line.startswith("TRANSPORTPROFILE "))
    transport_dist = int(profile.split()[1])
    route_lines = route.stdout.splitlines()
    header_index = next(i for i, line in enumerate(route_lines)
                        if line.startswith("ROUTE "))
    route_header = list(map(int, route_lines[header_index].split()[1:]))
    _, _unit_x, _unit_z, anchor_x, anchor_z, _order_count, _, _ = route_header
    endpoint_line = next(line for line in route_lines[header_index + 1:]
                         if len(line.split()) == 2)
    endpoint_x, endpoint_z = map(int, endpoint_line.split())
    route_cells = ((anchor_x // 65536 // 16, anchor_z // 65536 // 16),
                   (endpoint_x // 65536 // 16, endpoint_z // 65536 // 16))
    assert route_cells == ((240, 123), (240, 337)), route_cells

    route_checker = Path(__file__).with_name("check_surface_unload_map_route.py")
    checker_args = [sys.executable, str(route_checker), binary, "--retail-root",
                    str(root), "--map", MAP, "--start", *map(str, START),
                    "--target", *map(str, TARGET), "--carrier", CARRIER,
                    "--passenger", PASSENGER]
    if crusades:
        checker_args.append("--crusades")
    checked_route = run(*checker_args)
    if "unload route matches exactly" not in checked_route.stdout:
        raise AssertionError(checked_route.stdout)

    world = run(binary, "--surface-transport-map-roundtrip-type", str(root), MAP,
                CARRIER, PASSENGER, str(START[0]), str(START[1]), str(TARGET[0]),
                str(TARGET[1]), str(balance))
    maptravel = next(line for line in world.stdout.splitlines()
                     if line.startswith("MAPTRAVEL ")).split()
    _, start_x, start_z, goal_x, goal_z, world_unload_tick, carrier_x, carrier_z, _, _ = \
        (maptravel[0], *map(int, maptravel[-9:]))
    landing = next(line for line in world.stdout.splitlines()
                   if line.startswith("LANDING ")).split()
    world_land_x, world_land_z = float(landing[1]), float(landing[2])
    assert (start_x, start_z, goal_x, goal_z) == (3840, 1920, 3848, 5608)
    assert "retail carrier footprint is passable at release" in world.stdout
    assert "retail passenger footprint is placeable at release" in world.stdout
    assert "carrier cargo is empty after release" in world.stdout
    assert "remains physically valid through the 60-tick post-release coast" in world.stdout
    # This World fixture reads position on the first tick after transfer. The
    # passenger has already advanced under its new PARK order, so this sample
    # can cross a footprint-cell boundary even though the release itself uses
    # the exact authored center.
    world_after_release_site = (
        footprint_origin(world_land_x, passenger_profile[0]),
        footprint_origin(world_land_z, passenger_profile[1]))
    assert math.hypot(world_land_x - goal_x, world_land_z - goal_z) < 8, \
        (world_land_x, world_land_z, (goal_x, goal_z))

    native_place = native_placement_oracle(map_data, passenger_profile)
    placement_results = []

    def checked_placement(args, call_number):
        result = native_place(args, call_number)
        placement_results.append((args, result))
        return result

    native = SurfaceUnload(placement_result=checked_placement, freeze_hooks=False)
    put = lambda address, fmt, *values: native.p.uc.mem_write(
        address, struct.pack(fmt, *values))
    carrier_foot_x = carrier_foot_z = 4
    put(native.kind + 0x126, "<hh", carrier_foot_x, carrier_foot_z)
    put(native.carrier + 0x78, "<hh", carrier_foot_x, carrier_foot_z)
    put(native.kind + 0x23E, "<H", transport_dist)
    put(native.mission + 0x22, "<i", goal_x << 16)
    put(native.mission + 0x2A, "<i", goal_z << 16)
    initial = native.dispatch(1)
    assert initial[0:3] == (1, 1, 0x701), initial
    goal_cell = (math.floor((goal_x - (carrier_foot_x - 1) * 8) / 16),
                 math.floor((goal_z - (carrier_foot_z - 1) * 8) / 16))
    circle_radius = transport_dist - 34
    assert (native.controller_goal()[1], native.controller_goal()[2]) == \
        (goal_cell, circle_radius), native.controller_goal()

    # Deliver the route result from the paired trace, at World physical arrival.
    put(native.carrier + 0x68, "<iii", carrier_x << 16, 0, carrier_z << 16)
    put(native.carrier + 0x74, "<hh",
        footprint_origin(carrier_x, carrier_foot_x),
        footprint_origin(carrier_z, carrier_foot_z))
    put(native.nav + 0x10C, "<I", len(route_cells))
    put(native.nav + 0x110, "<I", len(route_cells))
    native.p.uc.mem_write(native.nav + 0x114, b"\x01")
    native.p.uc.mem_write(native.nav + 12, struct.pack("<4h", *route_cells[0],
                                                       *route_cells[1]))
    events, error = native.p.call(0x4E5150, ecx=native.nav)
    if error:
        raise RuntimeError(error)
    assert events & 0x100 and native.get(native.nav + 4) == 0, (events, native.get(native.nav + 4))
    transfer = native.dispatch(2)
    assert transfer[0:3] == (1, 2, 1), transfer
    release_tick = None
    for tick in range(3, 32):
        row = native.dispatch(tick)
        if native.get(native.carrier + 0xAC) == 0:
            release_tick = tick
            break
        if not row[0]:
            break
    assert release_tick is not None, "native GROUND_UNLOAD did not detach the passenger"
    assert placement_results and all(result == 1 for _, result in placement_results), \
        placement_results
    assert native.get(native.passenger + 0xA8) == 0
    released_position = struct.unpack("<3i", native.p.uc.mem_read(native.passenger + 0x68, 12))
    native_site = (footprint_origin(released_position[0] / 65536,
                                    passenger_profile[0]),
                   footprint_origin(released_position[2] / 65536,
                                    passenger_profile[1]))
    placements = native.placementCalls
    assert placements and all(struct.unpack("<hh", struct.pack("<I", call[2])) ==
                              TARGET for call in placements), placements
    world_requested_site = (footprint_origin(goal_x, passenger_profile[0]),
                            footprint_origin(goal_z, passenger_profile[1]))
    assert native_site == world_requested_site == TARGET, \
        (native_site, world_requested_site, TARGET)
    assert released_position[0] == goal_x << 16 and released_position[2] == goal_z << 16, \
        released_position
    retired = native.dispatch(release_tick + 1)
    assert retired[0] == 0 and native.parked, retired
    print(f"PASS: {'Crusades' if crusades else 'standard'} Lake Lokken {CARRIER}/{PASSENGER}: ")
    print(f"  paired route {route_cells[0]} -> {route_cells[1]}, unload circle goal "
          f"{goal_cell}, radius {circle_radius}px")
    print(f"  World release tick {world_unload_tick}, passenger at "
          f"({world_land_x:.2f},{world_land_z:.2f}) after its first PARK step "
          f"(footprint {world_after_release_site}); requested release cell {world_requested_site}.")
    print(f"  native 0x507d10 accepted cell {TARGET} on real TNT terrain, then retail "
          f"released at ({released_position[0] / 65536:.1f},"
          f"{released_position[2] / 65536:.1f}) on native transfer tick {release_tick}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/transport_test")
    parser.add_argument("--hpitool", default="build/hpitool")
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    args = parser.parse_args()
    binary, hpitool, root = (str(Path(value).resolve()) for value in
                             (args.binary, args.hpitool, args.retail_root))
    for crusades in (False, True):
        paired_check(binary, Path(root), hpitool, crusades)


if __name__ == "__main__":
    main()
