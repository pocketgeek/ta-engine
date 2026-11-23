#!/usr/bin/env python3
"""Compare native cached-footprint terrain grades with Lake Lokken World grades.

Native 0x508cd0/0x5088f0 score terrain directly from TNT-derived retail cell
records. This checks the Vertrans WATER4 route-origin corridor against the
World attempt plane in both balances. Off-route samples can differ because
World's effective search grades apply the grade-5 unexplored-terrain rule.

This is a terrain-grade probe: the native feature table is zero-filled while
TNT feature IDs remain in each cell record, so it does not certify arbitrary
map-feature blocking or dynamic unit occupancy.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

from balance_inputs import class_record, properties, unit_properties
from check_surface_unload_map_release import cat, parse_tnt
from emu import HEAP, Icd


MAP = "Lake Lokken"
START = (240, 120)
TARGET = (240, 350)
CARRIER = "vertrans"
PASSENGER = "araarch"
ROUTE_ORIGIN_X = 240
ROUTE_ORIGIN_Z = 121
ROUTE_ENDPOINT_Z = 337


def asset(hpitool, root, internal):
    where = subprocess.run([hpitool, "where", root, internal], check=True,
                           capture_output=True, text=True).stdout.strip()
    archive_path = where.split(" -> ", 1)[1]
    archive, member = archive_path.split("!", 1)
    return subprocess.run([hpitool, "cat", str(Path(root) / archive), member],
                          check=True, capture_output=True).stdout


def native_water_profile(hpitool, root, carrier="vertrans"):
    unit = unit_properties(
        asset(hpitool, root, f"units/{carrier.lower()}.fbi").decode("latin1"))
    movement = unit.get("movementclass", "").lower()
    if not movement.startswith("water"):
        raise AssertionError(f"{carrier} is not a water carrier: {movement}")
    moveinfo = asset(hpitool, root, "gamedata/moveinfo.tdf").decode("latin1")
    definitions = [properties(block) for block in
                   re.findall(r"\[[^]]+\]\s*\{([^{}]*)\}", moveinfo, re.S)]
    fields = next((record for record in definitions
                   if record.get("name", "").lower() == movement), None)
    if fields is None:
        raise AssertionError(f"MOVEINFO lacks {movement}")
    packed = class_record(fields)
    values = struct.unpack("<6h4B", packed)
    foot_x, foot_z = values[:2]
    if foot_x < 1 or foot_z < 1:
        raise AssertionError((carrier, movement, foot_x, foot_z))
    return movement, packed, foot_x, foot_z


def native_grade_reader(map_data, packed_profile):
    width, height, sea, heights, features, feature_count = map_data
    icd = Icd()
    uc = icd.uc
    game = HEAP + 0x10000
    grid = 0x72000000
    cells = 0x73000000
    feature_table = cells + width * height * 14 + 0x1000
    uc.mem_map(grid, 0x1000)
    uc.mem_map(cells, 0x400000)

    def put(address, fmt, *values):
        uc.mem_write(address, struct.pack(fmt, *values))

    put(0x62D55C, "<I", game)
    put(game + 0x19E98, "<I", width)
    put(game + 0x19E9C, "<I", height)
    put(game + 0x19F04, "<I", cells)
    put(game + 0x19EDC, "<I", feature_table)
    put(game + 0x19EC0, "<I", feature_count)
    uc.mem_write(game + 0x19EF8, bytes((sea,)))

    records = bytearray(width * height * 14)
    for z in range(height):
        for x in range(width):
            index = z * width + x
            offset = index * 14
            corners = (heights[index],
                       heights[z * width + min(x + 1, width - 1)],
                       heights[min(z + 1, height - 1) * width + x],
                       heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)])
            records[offset + 4] = heights[index]
            records[offset + 5] = max(corners)
            records[offset + 6] = min(corners)
            struct.pack_into("<H", records, offset + 8, features[index])
    uc.mem_write(cells, bytes(records))
    # Terrain grading does not need the feature definition bodies. Keep the
    # actual TNT feature IDs in cell records, but do not pretend to validate
    # feature passability in this focused probe.
    uc.mem_write(feature_table, bytes(feature_count * 320))

    foot_x, foot_z, max_depth, min_depth, bad_max, bad_min, max_slope, bad_slope, \
        max_water_slope, bad_water_slope = struct.unpack("<6h4B", packed_profile)
    put(grid + 4, "<hh", foot_x, foot_z)
    put(grid + 8, "<4h4B", max_depth, min_depth, bad_max, bad_min,
        max_slope, bad_slope, max_water_slope, bad_water_slope)

    def grade(x, z):
        result, error = icd.call(0x508CD0, (grid, x, z))
        if error:
            raise RuntimeError(f"native 0x508cd0 at {(x, z)}: {error}")
        return result

    return grade


def world_attempt(binary, root, hpitool, balance):
    env = {**os.environ, "TAK_DUMP_GRADE_PLANE": "1",
           "TAK_DUMP_ATTEMPT_PLANES": "1", "TAK_DUMP_ROUTE_ATTEMPT": "1"}
    command = [binary, "--surface-unload-map-route-type", root, MAP,
               CARRIER, PASSENGER, str(START[0]), str(START[1]),
               str(TARGET[0]), str(TARGET[1]), str(int(balance))]
    result = subprocess.run(command, check=True, capture_output=True, text=True, env=env)
    stderr = result.stderr.splitlines()
    attempt_header_index = next(i for i, line in enumerate(stderr)
                                if line.startswith("ATTEMPTPLANE "))
    header = list(map(int, stderr[attempt_header_index].split()[1:]))
    tick, unit_id, start_x, start_z, retry, heading, weight, traffic_radius, \
        width, height, foot_x, foot_z = header
    grades = [list(map(int, line.split())) for line in
              stderr[attempt_header_index + 1:attempt_header_index + 1 + height]]
    if len(grades) != height or any(len(row) != width for row in grades):
        raise AssertionError("incomplete World attempt grade plane")

    profile = next(line for line in stderr if line.startswith("ATTEMPTPROFILE "))
    profile_values = list(map(int, profile.split()[1:]))
    p_tick, p_unit, _, pfx, pfz = profile_values[:5]
    if (p_tick, p_unit, pfx, pfz) != (tick, unit_id, foot_x, foot_z):
        raise AssertionError((header, profile_values))
    raw = [tuple(map(int, line.split()[2:])) for line in stderr
           if line.startswith("WORLDRAW ")]
    if raw != [(240, 123), (240, ROUTE_ENDPOINT_Z)]:
        raise AssertionError(("World route-origin requests", raw))
    if (start_x, start_z, retry, foot_x, foot_z) != \
            (ROUTE_ORIGIN_X, ROUTE_ORIGIN_Z, 0, 4, 4):
        raise AssertionError(("World attempt profile", header))
    return (tick, grades)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/transport_test")
    parser.add_argument("--hpitool", default="build/hpitool")
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    args = parser.parse_args()
    binary, hpitool, root = (str(Path(p).resolve()) for p in
                             (args.binary, args.hpitool, args.retail_root))

    map_data = parse_tnt(cat(hpitool, Path(root), "maps.hpi", "Maps/Lake Lokken.tnt"))
    width, height, sea, *_ = map_data
    if (width, height, sea) != (480, 480, 58):
        raise AssertionError((width, height, sea))
    movement, profile, foot_x, foot_z = native_water_profile(hpitool, root)
    native_grade = native_grade_reader(map_data, profile)

    for crusades in (False, True):
        tick, world = world_attempt(binary, root, hpitool, crusades)
        if len(world) != height:
            raise AssertionError((len(world), height))

        corridor = [(ROUTE_ORIGIN_X, z) for z in
                    range(ROUTE_ORIGIN_Z, ROUTE_ENDPOINT_Z + 1)]
        comparisons = [(x, z, world[z][x], native_grade(x, z))
                       for x, z in corridor]
        differences = [row for row in comparisons if row[2] != row[3]]
        if differences:
            raise AssertionError(("native/World route corridor grade mismatch", differences[:20]))

        # Probe a narrow surrounding band to make the World/native boundary
        # visible. Its effective grade 5 is the searcher's unexplored-terrain
        # fallback; it is not a terrain grade produced by 0x508cd0.
        off_route = [(x, z) for z in range(116, 352) for x in range(236, 246)
                     if x != ROUTE_ORIGIN_X]
        off_route_pairs = [(x, z, world[z][x], native_grade(x, z))
                           for x, z in off_route]
        off_route_differences = [row for row in off_route_pairs if row[2] != row[3]]
        if any(world_grade != 5 for _, _, world_grade, _ in off_route_differences):
            raise AssertionError(("off-route discrepancy outside grade-5 visibility fallback",
                                  off_route_differences[:20]))

        print(f"PASS {'Crusades' if crusades else 'Standard'}: native {movement} "
              f"0x508cd0 grades match World exactly on {len(corridor)} route-origin "
              f"cells at attempt tick {tick}; endpoint (240,337) grade "
              f"{native_grade(240, 337)}; 0 threshold differences.")
        print(f"  Off-route sample: {len(off_route_differences)}/"
              f"{len(off_route)} grade values differ; every differing World value is 5, "
              "the unexplored-terrain search grade.")
    print("  Profile comes from Vertrans WATER4 MOVEINFO (4x4). Native cell heights "
          "and feature IDs come from Lake Lokken TNT; feature definition bodies "
          "are zero-filled, so this validates terrain grades, not feature blocking.")


if __name__ == "__main__":
    main()
