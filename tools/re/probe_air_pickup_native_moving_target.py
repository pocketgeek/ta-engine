#!/usr/bin/env python3
"""Drive retail VTOL pickup while its passenger moves across Lake Lokken.

This extends the existing native full-map pickup fixture with a bounded moving
target. Retail builds height sectors from the shipped Lake Lokken TNT, creates
the real VTOL_Pickup / Move_Seek_Pickup orders, and runs the native dispatcher,
flight controller, mover, attachment and BeCarried transition. Araarch's path
is a host-controlled glide along a verified, passable shoreline strip;
the passenger ground pathfinder/mover is intentionally not part of this test.

The trajectory stays within one native 128px body sector so its genuine sector
list link remains valid as the passenger moves. The target moves 112px east
over 800 ticks, then waits for ZONROC to catch it. This checks that the native
air pickup pursues the passenger's current position rather than its stale
initial point. No retail GUI is launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_moving_target.py
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_moving_target.py --crusades
"""
import argparse
import struct
import subprocess
from pathlib import Path

from balance_inputs import unit_properties
from check_surface_unload_map_release import cat, movement_profile, native_placement_oracle
from check_surface_unload_map_grades import asset as retail_asset
from probe_air_pickup_native_fullmap import (
    DEFAULT_CARRIER, DEFAULT_PASSENGER, START, SCALE,
    fixed, init_orders_and_units, make_map, native_half_cell_ticks, put, u32,
)
from probe_transport_air_unload_map_flight import carrier_profile
from emuphase import GS, Phase


MOVING_START_X = 240
MOVING_END_X = 247
MOVING_Z = 350
MOVE_TICKS = 800


def unit_profile(hpitool, root, unit, crusades):
    internal = f"units/{unit.lower()}.fbi"
    if crusades:
        try:
            text = retail_asset(hpitool, root,
                                f"unitscb/{unit.lower()}.fbi").decode("latin1")
        except subprocess.CalledProcessError:
            text = retail_asset(hpitool, root, internal).decode("latin1")
    else:
        text = retail_asset(hpitool, root, internal).decode("latin1")
    return unit_properties(text)


def apply_carrier_profile(live, profile, heights, width):
    uc = live["uc"]
    carrier, kind = live["carrier"], live["carrier_kind"]
    put(uc, carrier + 0x12B, fixed(profile["maxvelocity"]))
    put(uc, kind + 0x162, fixed(profile["maxvelocity"]))
    put(uc, kind + 0x166, fixed(profile["brakerate"]))
    put(uc, kind + 0x16A, fixed(profile["acceleration"]))
    put(uc, kind + 0x16E, fixed(profile["watermultiplier"]))
    put(uc, kind + 0x172, fixed(profile["roadmultiplier"]))
    uc.mem_write(kind + 0x18E, struct.pack("<H", profile["turnrate"]))
    uc.mem_write(kind + 0x23A, struct.pack("<h", profile["cruisealt"]))
    uc.mem_write(kind + 0x23E, struct.pack("<H", profile["transportdistance"]))
    footprint = profile["footprintx"] | (profile["footprintz"] << 16)
    put(uc, kind + 0x126, footprint)
    put(uc, carrier + 0x78, footprint)
    x, z = START
    carrier_y = heights[z * width + x] + profile["cruisealt"]
    put(uc, carrier + 0x6C, carrier_y * 65536)
    put(uc, live["carrier_kind"] + 0x249,
        native_half_cell_ticks(profile))
    put(uc, live["carrier_nav"] + 0x0C,
        x * SCALE * 65536, carrier_y * 65536, z * SCALE * 65536)


def validate_passenger_strip(hpitool, root, map_data, passenger_info):
    """Require every trajectory footprint to be land-placeable on shipped TNT."""
    movement_info = cat(hpitool, root, "data.hpi",
                        "gamedata/moveinfo.tdf").decode("latin1")
    profile = movement_profile(
        movement_info,
        cat(hpitool, root, "data.hpi",
            f"units/{DEFAULT_PASSENGER}.fbi").decode("latin1"))
    can_place = native_placement_oracle(map_data, profile)
    for x in range(MOVING_START_X, MOVING_END_X + 1):
        packed = ((MOVING_Z & 0xFFFF) << 16) | (x & 0xFFFF)
        result = can_place((0, 2, packed, 1, 0), None)
        if not result:
            raise AssertionError(("moving passenger route is not placeable",
                                  x, MOVING_Z, profile))
    return profile


def run(args):
    root = Path(args.retail_root).resolve()
    hpitool = Path(args.hpitool).resolve()
    phase = Phase(480, 480)
    map_data, sectors, stride = make_map(phase, root, hpitool)
    width, height, sea, heights, _feature_ids, _feature_count = map_data
    carrier_name, passenger_name = args.carrier.lower(), args.passenger.lower()
    live = init_orders_and_units(phase, map_data, sectors, stride,
                                 carrier_name, passenger_name)
    uc, icd = live["uc"], live["icd"]
    carrier, passenger = live["carrier"], live["passenger"]
    carrier_mover, carrier_nav = live["carrier_mover"], live["carrier_nav"]
    carrier_order = live["carrier_order"]

    selected_carrier = carrier_profile(hpitool, root, carrier_name, args.crusades)
    selected_passenger = unit_profile(hpitool, root, passenger_name, args.crusades)
    if selected_passenger.get("movementclass", "").lower() != "ground2":
        raise AssertionError(("fixture passenger no longer uses GROUND2",
                              passenger_name,
                              selected_passenger.get("movementclass")))
    passenger_profile = validate_passenger_strip(
        hpitool, root, map_data, selected_passenger)
    apply_carrier_profile(live, selected_carrier, heights, width)
    passenger_footprint = passenger_profile[0] | (passenger_profile[1] << 16)
    put(uc, live["passenger_kind"] + 0x126, passenger_footprint)
    put(uc, passenger + 0x78, passenger_footprint)

    # The passenger's 2x2 Araarch footprint walks along the authored z=350
    # shoreline. This remains inside x-sector 30/z-sector 43 for its full
    # trajectory.
    target_y = heights[MOVING_Z * width + MOVING_START_X]
    strip_heights = [heights[MOVING_Z * width + x]
                     for x in range(MOVING_START_X,
                                    MOVING_END_X + passenger_profile[0])]
    if max(strip_heights) - min(strip_heights) > 5:
        raise AssertionError(("selected passenger strip exceeds its height bound",
                              min(strip_heights), max(strip_heights)))
    passenger_x_raw = MOVING_START_X * SCALE * 65536
    passenger_z_raw = MOVING_Z * SCALE * 65536
    put(uc, passenger + 0x68, passenger_x_raw, target_y * 65536,
        passenger_z_raw)
    uc.mem_write(passenger + 0x74, struct.pack("<hh", MOVING_START_X,
                                               MOVING_Z))
    passenger_sector = sectors + ((MOVING_Z // 8) * stride +
                                  MOVING_START_X // 8) * 10
    if u32(uc, passenger + 0xA4) != passenger_sector:
        raise AssertionError(("passenger body sector does not match path",
                              hex(u32(uc, passenger + 0xA4)),
                              hex(passenger_sector)))

    settings = phase._alloc(0x1000)
    options = settings + 0x100
    put(uc, 0x62D558, settings)
    put(uc, settings, options)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))

    initial_target = (passenger_x_raw, target_y * 65536, passenger_z_raw)
    max_ticks = args.max_ticks
    if not 1 <= max_ticks <= 10000:
        raise ValueError("--max-ticks must be in 1..10000")
    target_positions = []
    controller_goals = []
    attachment_tick = None
    controller_seen = False
    carrier_retired_tick = None

    def sample_target(tick):
        progress = min(tick, MOVE_TICKS) / MOVE_TICKS
        world_x = (MOVING_START_X * SCALE +
                   (MOVING_END_X - MOVING_START_X) * SCALE * progress)
        x_raw = int(world_x * 65536)
        cell_x = min(MOVING_END_X,
                     max(MOVING_START_X, int(world_x // SCALE)))
        y_raw = heights[MOVING_Z * width + cell_x] * 65536
        z_raw = MOVING_Z * SCALE * 65536
        put(uc, passenger + 0x68, x_raw, y_raw, z_raw)
        uc.mem_write(passenger + 0x74,
                     struct.pack("<hh", cell_x, MOVING_Z))
        return (x_raw, y_raw, z_raw, cell_x)

    for tick in range(1, max_ticks + 1):
        target = sample_target(tick)
        target_positions.append(target)
        put(uc, GS + 0x19F44, tick)
        _, error = icd.call(0x4D8450, (carrier,))
        if error:
            raise RuntimeError(("native VTOL pickup dispatcher", tick, error))
        controller = u32(uc, carrier_nav + 4)
        if controller:
            controller_seen = True
            goal = struct.unpack("<3i", uc.mem_read(controller + 0x26, 12))
            controller_goals.append((tick, goal))
        _, error = icd.call(0x4DC800, (carrier,), ecx=carrier_mover)
        if error:
            raise RuntimeError(("native flight mover", tick, error))
        _, error = icd.call(0x51B2A0, (carrier,), ecx=carrier_mover)
        if error:
            raise RuntimeError(("native flight height commit", tick, error))

        carrier_xyz = struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))
        if u32(uc, carrier + 0xAC) == passenger and \
                u32(uc, passenger + 0xA8) == carrier:
            if attachment_tick is None:
                attachment_tick = tick
        if attachment_tick is not None and u32(uc, carrier + 0x60) == 0:
            carrier_retired_tick = tick
            break

    if not controller_seen:
        raise AssertionError("native VTOL pickup did not create a flight controller")
    if attachment_tick is None:
        raise AssertionError(("moving-target native pickup did not attach",
                              max_ticks, "carrier XYZ", carrier_xyz,
                              "target", target_positions[-1]))
    if carrier_retired_tick != attachment_tick:
        raise AssertionError(("native carrier pickup did not retire on attach",
                              attachment_tick, carrier_retired_tick))
    if u32(uc, carrier + 0xAC) != passenger or \
            u32(uc, passenger + 0xA8) != carrier:
        raise AssertionError("native cargo links are not reciprocal")
    carried_order = u32(uc, passenger + 0x60)
    if not carried_order or uc.mem_read(carried_order + 4, 1)[0] != 11:
        raise AssertionError(("native BeCarried order was not installed",
                              hex(carried_order)))
    if target_positions[MOVE_TICKS - 1][0] <= initial_target[0]:
        raise AssertionError("passenger target did not move from its initial position")
    final_target = target_positions[attachment_tick - 1]
    if final_target[0] < initial_target[0] + 100 * 65536:
        raise AssertionError(("pickup occurred before target moved far enough",
                              attachment_tick,
                              initial_target[0] // 65536,
                              final_target[0] // 65536))
    if not controller_goals or controller_goals[-1][1][0] < \
            initial_target[0] + 100 * 65536:
        raise AssertionError(("native flight controller did not follow passenger x",
                              controller_goals[:3], controller_goals[-3:],
                              initial_target))

    carrier_x, carrier_y, carrier_z = carrier_xyz
    final_distance = ((carrier_x - final_target[0]) ** 2 +
                     (carrier_z - final_target[2]) ** 2) ** 0.5 / 65536
    print(f"PASS: Lake Lokken {carrier_name} ({'Crusades' if args.crusades else 'Standard'}) "
          f"tracked moving Araarch through {attachment_tick - 1} native flight "
          f"updates, attached on tick {attachment_tick}, and retired its pickup order.")
    print(f"  Target: x={initial_target[0] / 65536:.1f}->{final_target[0] / 65536:.1f}px, "
          f"z={final_target[2] / 65536:.1f}px over {MOVE_TICKS} ticks; "
          f"final carrier-to-target planar distance={final_distance:.1f}px; "
          f"native sectors={stride}x{stride}; Araarch footprint="
          f"{passenger_profile[0]}x{passenger_profile[1]}.")
    print(f"  Native flight target samples: first={controller_goals[:1]}, "
          f"last={controller_goals[-1:]}")
    print("  Retail-controlled: TNT height-sector builder, native VTOL pickup "
          "dispatcher and flight mover, cargo attachment, order transition and "
          "BeCarried dispatch. Host-controlled: Araarch's smooth 112px movement "
          "along the map-validated shoreline strip, movement clock, visibility, UI/effect "
          "and empty COB-method boundary. This does not test the passenger's "
          "ground pathfinder/mover or a map-wide flying route search.")


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo / "build-o2/hpitool"))
    parser.add_argument("--carrier", default=DEFAULT_CARRIER)
    parser.add_argument("--passenger", default=DEFAULT_PASSENGER)
    parser.add_argument("--max-ticks", type=int, default=5000)
    parser.add_argument("--crusades", action="store_true")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
