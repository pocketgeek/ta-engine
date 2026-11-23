#!/usr/bin/env python3
"""Join Lake Lokken's native GROUND_PICKUP route to mover and boarding.

The route is created by retail's 0x4e54e0/0x416430 path and remains installed
in the real native navigator while 0x4dc800 + 0x51b2a0 advance its Vertrans
mover across TNT-backed terrain. Retail's 0x4d8450 dispatcher consumes native
arrival, runs the pickup transfer, attaches the Araarch, and retires the
carrier mission. A World load-order roundtrip on the same shipped map is run as
an independent port-level control.

Run from the repository root (no retail GUI is launched):
    python3 tools/re/probe_surface_pickup_native_mission.py
"""
import argparse
import re
import struct
import subprocess
from pathlib import Path

from emuphase import GS
from probe_surface_pickup_native_map_route import MAP, CARRIER, PASSENGER, START, TARGET, run


def fixed(value):
    return int(round(float(value) * 65536))


def configure_map_mover(state):
    phase, live, uc = state["phase"], state["live"], state["phase"].uc
    width, height, sea = state["width"], state["height"], state["sea"]
    carrier, mover, kind, owner = live.carrier, live.mover, live.kind, live.owner
    heights, feature_ids, feature_count = state["map_data"][3], state["map_data"][4], state["feature_count"]
    fields = state["carrier_fbi"]
    fx, fz = state["foot_x"], state["foot_z"]

    # Mirror the native-map mover setup used by the paired unload trace: raw
    # corner-height/feature records, 128px sector maxima, visible map, and the
    # actual WATER4 limits. This is specifically for the mover's terrain scans;
    # the route itself already came from native 0x508cd0 grades.
    sector_stride = (width + 7) // 8
    sector_rows = (height + 7) // 8
    sector_records = bytearray(sector_stride * sector_rows * 10)
    for sz in range(sector_rows):
        for sx in range(sector_stride):
            block = [heights[z * width + x]
                     for z in range(sz * 8, min(height, sz * 8 + 8))
                     for x in range(sx * 8, min(width, sx * 8 + 8))]
            sector_records[(sz * sector_stride + sx) * 10 + 1] = max(block)
    sector_grid = phase._alloc(len(sector_records))
    uc.mem_write(sector_grid, bytes(sector_records))
    live.put(GS + 0x19F18, sector_grid)
    live.put(GS + 0x19F1C, sector_stride)
    sector_end = sector_grid + len(sector_records)

    visibility = live.get(GS + 0x19EF4)
    uc.mem_write(visibility, struct.pack("<" + "H" * (width * height // 4),
                                        *([0xFFFF] * (width * height // 4))))
    live.put(owner + 0x8C, width // 2)
    live.put(owner + 0x90, height // 2)
    live.byte(GS + 0x306F, 0)
    live.put(GS + 0x19EF8, sea)

    max_velocity = fixed(fields.get("maxvelocity", 0))
    braking = fixed(fields.get("brakerate", 0.5))
    acceleration = fixed(fields.get("acceleration", 0.5))
    turn_rate = int(float(fields.get("turnrate", 150)))
    waterline = int(float(fields.get("waterline", 1)))
    live.put(carrier + 0x12B, max_velocity)
    live.put(carrier + 0x68, START[0] * 16 * 65536)
    live.put(carrier + 0x6C, sea * 65536)
    live.put(carrier + 0x70, START[1] * 16 * 65536)
    live.put(carrier + 0x7E, 0)
    # +0xa4 is the carrier's current sector-list node, not the sector-record
    # address. Leave it clear so 0x4dc800 performs the initial insertion itself.
    live.put(mover + 0x20, 0)
    live.put(mover + 0x30, 0x7FFFFFFF)
    live.put(mover + 8, 0)
    live.put(mover + 0x14, 0)
    live.put(mover + 0x36, 0x1000 | 1)
    live.put(kind + 0x162, max_velocity)
    live.put(kind + 0x166, braking)
    live.put(kind + 0x16A, acceleration)
    live.put(kind + 0x126, fx | (fz << 16))
    live.put(kind + 0x18A, phase.GRID)
    live.put(kind + 0x18E, turn_rate)
    live.put(kind + 0x248, waterline)
    live.byte(kind + 0x24A, 1)
    live.byte(kind + 0x249, 70)
    live.put(phase.GRID + 4, fx | (fz << 16))
    max_depth, min_depth, bad_max_depth, bad_min_depth, max_slope, bad_slope, \
        max_water_slope, bad_water_slope = state["profile_limits"]
    uc.mem_write(phase.GRID + 8, struct.pack(
        "<4h4B", max_depth, min_depth, bad_max_depth, bad_min_depth,
        max_slope, bad_slope, max_water_slope, bad_water_slope))

    # The real game creates this singleton during startup. Route delivery has
    # already completed; the native mover can still ask it to refresh a segment
    # at a waypoint boundary.
    search_service = phase._alloc(0x22B)
    _, error = phase.icd.call(0x415F80, (), ecx=search_service)
    assert error is None, ("native route-search service constructor", error)
    live.put(GS + 0x19E70, search_service)
    assert not (sector_grid <= live.get(live.nav + 4) < sector_end), (
        "native pickup controller overlaps sector table",
        hex(live.get(live.nav + 4)), hex(sector_grid), hex(sector_end))


def run_native(state, max_ticks):
    phase, live, uc = state["phase"], state["live"], state["phase"].uc
    carrier, passenger, nav, mover = live.carrier, live.passenger, live.nav, live.mover
    configure_map_mover(state)
    if state.get("native_attachment"):
        # 0x51b5a0 removes the passenger from its 128px sector occupancy list
        # through passenger+0xa4. Give the stationary Araarch a genuine native
        # sector-list membership via 0x506650; leaving this pointer null would
        # make the real attachment routine dereference address 0x6 in
        # 0x5065f0. The grid itself contains native-format records built above.
        sector_grid = live.get(GS + 0x19F18)
        sector_stride = live.get(GS + 0x19F1C)
        sector_x, sector_z = TARGET[0] // 8, TARGET[1] // 8
        passenger_sector = sector_grid + (sector_z * sector_stride + sector_x) * 10
        _, error = phase.icd.call(0x506650, (passenger, passenger_sector))
        assert error is None, ("native Araarch sector insertion", error)
        assert live.get(passenger + 0xA4) == passenger_sector
        assert live.get(passenger_sector + 6) == passenger
    # Point the ground target at the authored map location. The route probe's
    # dispatch used this same object position when it installed the circle.
    live.put(passenger + 0x68, TARGET[0] * 16 * 65536)
    live.put(passenger + 0x6C, state["sea"] * 65536)
    live.put(passenger + 0x70, TARGET[1] * 16 * 65536)

    trace = []
    stage_seen = set()
    attached_tick = None
    arrival_tick = None
    for step in range(1, max_ticks + 1):
        tick = state["delivered_at"] + step
        live.put(GS + 0x19F44, tick)
        # Move the target values in the native reciprocal Move_Seek_Pickup
        # object as a stationary ground unit would be represented.
        row = live.dispatch(tick)
        if (live.get(carrier + 0xAC) == passenger and
                live.get(passenger + 0xA8) == carrier):
            attached_tick = tick
        assert row["active"] or attached_tick is not None, (
            "carrier mission retired before attachment", step, row, trace[-8:],
            {"position": struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12)),
             "events": live.get(live.mission + 0x6A),
             "stage": uc.mem_read(live.mission + 5, 1)[0],
             "nav_count": live.get(nav + 0x10C),
             "controller": live.get(nav + 4),
             "cargo": live.get(carrier + 0xAC),
             "passenger_parent": live.get(passenger + 0xA8),
             "stage_seen": sorted(stage_seen)})
        if row["active"]:
            stage_seen.add(row["stage"])
            if row["stage"] == 2 and arrival_tick is None:
                arrival_tick = tick

        # Retail's native unit mover and nav-controller update run after mission
        # dispatch. The controller posts 0x100 here; the next dispatcher poll
        # consumes it and enters the transfer stage.
        _, error = phase.icd.call(0x4DC800, (carrier,), ecx=mover)
        assert error is None, ("native Vertrans mover", step, error)
        _, error = phase.icd.call(0x51B2A0, (carrier,), ecx=mover)
        assert error is None, ("native pickup nav update", step, error)

        pos = struct.unpack("<iii", uc.mem_read(carrier + 0x68, 12))
        mission_ptr = live.get(carrier + 0x60)
        mission_stage = uc.mem_read(live.mission + 5, 1)[0] if mission_ptr else -1
        events = live.get(live.mission + 0x6A) if mission_ptr else 0
        cargo = live.get(carrier + 0xAC)
        passenger_parent = live.get(passenger + 0xA8)
        if cargo == passenger and passenger_parent == carrier:
            attached_tick = tick
        trace.append((tick, pos[0] // 65536, pos[2] // 65536, mission_stage,
                      events, live.get(nav + 0x10C), int(cargo == passenger),
                      int(passenger_parent == carrier)))

        # Once attach and carrier-order retirement have both been observed,
        # allow one more dispatcher tick to verify the queue stays retired.
        if attached_tick is not None and mission_ptr == 0:
            live.put(GS + 0x19F44, tick + 1)
            retired = live.dispatch(tick + 1)
            assert not retired["active"] and live.get(carrier + 0x60) == 0, retired
            break

    assert attached_tick is not None, {
        "ticks": max_ticks, "trace_tail": trace[-5:],
        "mission": live.dispatch(state["delivered_at"] + max_ticks),
        "stage_seen": sorted(stage_seen), "events": live.get(live.mission + 0x6A),
        "controller": hex(live.get(nav + 4)),
        "waypoints": live.get(nav + 0x10C),
        "carrier": struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12)),
    }
    assert not live.get(carrier + 0x60), "carrier pickup order did not retire after boarding"
    if not state.get("native_attachment"):
        assert live.get(passenger + 0x60) == 0, "native passenger pickup order did not retire"
    assert live.get(carrier + 0xAC) == passenger, "native carrier cargo link is missing"
    assert live.get(passenger + 0xA8) == carrier, "native reciprocal cargo link is missing"
    assert 2 in stage_seen, ("native pickup never entered transfer stage", sorted(stage_seen))
    assert arrival_tick is not None and arrival_tick <= attached_tick, (
        arrival_tick, attached_tick, trace[-8:])
    assert not live.get(nav + 4), "native pickup controller remained installed after arrival"
    assert live.transfer_effects >= 2, ("native pickup transfer effects did not run",
                                       live.transfer_effects)
    return attached_tick, arrival_tick, trace


def run_world(binary, retail_root):
    command = [str(binary), "--surface-transport-map-roundtrip-type",
               str(retail_root), MAP, CARRIER, PASSENGER,
               str(START[0]), str(START[1]), str(TARGET[0]), str(TARGET[1])]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    pickup_match = re.search(r"PICKUP_TICK (\d+)", result.stdout)
    if not pickup_match or "MISSION 0" not in result.stdout:
        raise AssertionError(("World did not board and retire its carrier mission",
                              command, result.stdout[-4000:]))
    if "carrier completed routed shore unload" in result.stdout:
        raise AssertionError(("unexpected output parse", result.stdout[-4000:]))
    for expected in ("PASS: retail passenger boarded through the issued load order",
                     "PASS: boat completed routed shore unload on the shipped map",
                     "PASS: carrier cargo is empty after release"):
        assert expected in result.stdout, (expected, result.stdout[-4000:])
    return int(pickup_match.group(1)), result.stdout


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo / "build-o2/hpitool"))
    parser.add_argument("--world-binary", default=str(repo / "build-o2/transport_test"))
    parser.add_argument("--max-ticks", type=int, default=5000)
    args = parser.parse_args()

    retail_root = Path(args.retail_root).resolve()
    state = run(retail_root, Path(args.hpitool).resolve())
    native_tick, arrival_tick, native_trace = run_native(state, args.max_ticks)
    world_tick, world_output = run_world(Path(args.world_binary).resolve(), retail_root)
    print(
        f"PASS: native Lake Lokken Vertrans GROUND_PICKUP followed its installed "
        f"{len(state['route'])}-waypoint route through 0x4dc800/0x51b2a0; "
        f"arrival at tick {arrival_tick}, transfer effects, passenger attachment, "
        f"and carrier mission retirement completed at tick {native_tick}."
    )
    print(
        f"  World shipped-map load-order roundtrip also boards and retires its "
        f"carrier mission at tick {world_tick}; native mover ticks after route "
        f"delivery: {native_tick - state['delivered_at']} ({len(native_trace)} rows)."
    )
    print("  World terminal checks: boarded passenger, routed shore unload, empty cargo.")


if __name__ == "__main__":
    main()
