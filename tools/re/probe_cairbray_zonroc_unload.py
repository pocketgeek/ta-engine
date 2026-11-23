#!/usr/bin/env python3
"""Pair one Standard ZONROC/Araarch unload over Cairbray's actual TNT map.

World loads the shipped map and FBI profiles through the normal VFS/type path.
The retail side builds height sectors with 0x50e740, keeps 0x4dc800's terrain
scan and 0x4d8450 VTOL_UNLOAD dispatcher live, and lets native 0x507d10 decide
the Araarch placement. Heap allocation, script/UI effects, cargo detachment,
PARK installation, and native rand values remain the existing fixture sinks.
No GUI or production simulation code is involved.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

from emu import HEAP, HEAP_SZ
from check_surface_unload_map_release import cat, movement_profile, parse_tnt
from probe_transport_air_unload_map_flight import (
    apply_native_profile, carrier_profile, fixed,
)
from probe_transport_air_unload_flight import native_row
from probe_transport_air_unload_callbacks import NativeAirUnload
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP


MAP = "Cairbray Coast Landing"
START = (12, 153)
SITE = (97, 153)
EXIT = (182, 153)


def world_trace(binary, retail_root, start, site, steps):
    result = subprocess.run(
        [binary, "--air-unload-map-flight-trace", retail_root, MAP,
         "zonroc", "araarch", str(start[0]), str(start[1]),
         str(site[0]), str(site[1]), str(steps), "0"],
        check=True, capture_output=True, text=True)
    lines = result.stdout.splitlines()
    header = next(line for line in lines if line.startswith("MAPTRACE "))
    rows = [tuple(map(int, line.split())) for line in lines if line[:1].isdigit()]
    if len(rows) != steps or any(len(row) != 32 for row in rows):
        raise AssertionError(("World trace shape", len(rows), steps,
                              sorted({len(row) for row in rows})))
    return header, rows


def write_native_map(native, map_data):
    width, height, sea, heights, features, feature_count = map_data
    p, uc, game = native.p, native.p.uc, native.game
    cells = HEAP + 0x220000
    feature_table = HEAP + 0x1C0000
    units = HEAP + 0x3D0000
    native.put(game + 0x19E88, width * 16)
    native.put(game + 0x19E8C, height * 16)
    native.put(game + 0x19E98, width)
    native.put(game + 0x19E9C, height)
    native.byte(game + 0x19EF8, sea)
    native.put(game + 0x19EC0, feature_count)
    native.put(game + 0x19EDC, feature_table)
    native.put(game + 0x19F04, cells)
    native.put(game + 0x14E84, units)
    native.put(game + 0x14E88, units + 312 * 64)

    records = bytearray(width * height * 14)
    for z in range(height):
        for x in range(width):
            index = z * width + x
            offset = index * 14
            corners = (
                heights[index],
                heights[z * width + min(x + 1, width - 1)],
                heights[min(z + 1, height - 1) * width + x],
                heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)],
            )
            records[offset + 4] = heights[index]
            records[offset + 5] = max(corners)
            records[offset + 6] = min(corners)
            struct.pack_into("<H", records, offset + 8, features[index])
    uc.mem_write(cells, bytes(records))
    uc.mem_write(feature_table, bytes(max(320, feature_count * 320)))
    uc.mem_write(units, bytes(312 * 64))

    passenger_id = 42
    passenger_record = units + passenger_id * 312
    passenger_nav = HEAP + 0xB8000
    uc.mem_write(passenger_nav, bytes(0x180))
    native.put(passenger_record + 2, passenger_id)
    native.put(passenger_record + 8, passenger_nav)
    native.put(passenger_record + 0xB4, HEAP + 0xB0000)
    native.put(passenger_record + 0x130, 0x01000000)

    p.hooks = dict(p.hooks)
    original_allocator = p.hooks[0x4EB9E0]
    cursor = [HEAP + 0x310000]

    def allocate(machine, argp):
        size = struct.unpack("<I", machine.mem_read(argp, 4))[0]
        address = cursor[0]
        end = address + max(size, 1)
        if address < HEAP + 0x300000 or end > HEAP + HEAP_SZ:
            raise RuntimeError(("map allocation escaped scratch arena", hex(address),
                                size, hex(HEAP + HEAP_SZ)))
        machine.mem_write(address, bytes(size))
        cursor[0] = (end + 15) & ~15
        return 0, address

    p.hooks[0x4EB9E0] = allocate
    _, error = p.call(0x50E740)
    if error:
        raise RuntimeError(("retail 0x50e740 map-sector build", error))
    p.hooks[0x4EB9E0] = original_allocator
    sectors = native.get(game + 0x19F18)
    stride = native.get(game + 0x19F1C)
    expected_stride = (width + 7) // 8
    rows = (height + 7) // 8
    if (stride, rows) != (expected_stride, (height + 7) // 8):
        raise AssertionError(("retail sector dimensions", stride, rows,
                              expected_stride, (height + 7) // 8))

    raw = []
    for sector_z in range(rows):
        for sector_x in range(stride):
            raw.append(max(sea, *(max(
                heights[z * width + x],
                heights[z * width + min(x + 1, width - 1)],
                heights[min(z + 1, height - 1) * width + x],
                heights[min(z + 1, height - 1) * width +
                        min(x + 1, width - 1)])
                for z in range(sector_z * 8, min(height, sector_z * 8 + 8))
                for x in range(sector_x * 8, min(width, sector_x * 8 + 8)))))
    for sector_z in range(rows):
        for sector_x in range(stride):
            index = sector_z * stride + sector_x
            dilated = max(raw[nz * stride + nx]
                          for nz in range(max(0, sector_z - 1), min(rows, sector_z + 2))
                          for nx in range(max(0, sector_x - 1), min(stride, sector_x + 2)))
            actual = tuple(uc.mem_read(sectors + index * 10, 2))
            if actual != (raw[index], dilated):
                raise AssertionError(("native 0x50e740 record", sector_x, sector_z,
                                      actual, (raw[index], dilated)))
    return sectors, stride


def setup_native(root, hpitool, map_data, profile, passenger_profile, start, site,
                 world_rows):
    draws = {}
    previous_deadline = None
    for row in world_rows:
        tick, deadline = row[0], row[19]
        if row[17] == 1 and deadline and deadline != previous_deadline:
            draw = deadline - tick - 6
            if not 0 <= draw < 6:
                raise AssertionError(("unexpected VTOL poll interval", tick,
                                      deadline, draw))
            draws[tick] = draw
        previous_deadline = deadline

    native = NativeAirUnload(
        random_value=lambda bound, tick: draws.get(tick, 0) if bound == 6 else 0)
    p, uc = native.p, native.p.uc
    p.hooks = dict(p.hooks)
    apply_native_profile(native, profile)
    native.passenger_kind = HEAP + 0xB0000
    passenger_kind = native.passenger_kind
    width, height, sea, heights, features, feature_count = map_data
    start_x, start_z = start[0] * 16 + 8, start[1] * 16 + 8
    target_x, target_z = site[0] * 16 + 8, site[1] * 16 + 8
    start_y = int(heights[start[1] * width + start[0]])
    target_y = int(heights[site[1] * width + site[0]])

    footprint = profile["footprintx"] | (profile["footprintz"] << 16)
    native.put(native.kind + 0x12A, HEAP + 0xB9000)
    native.put(passenger_kind + 0x12A, HEAP + 0xBA000)
    uc.mem_write(HEAP + 0xB9000, bytes(0x1000))
    uc.mem_write(HEAP + 0xBA000, bytes(0x1000))
    # 0x507d10 branches on UnitDef+0x24a. Mobile units set this byte to one;
    # without it, ZONROC is checked as a building and the body update falsely
    # rejects each changed cell, entering retail's collision-braking path.
    native.byte(native.kind + 0x24A, 1)
    native.put(native.kind + 0x126, footprint)
    native.put(native.carrier + 0x78, footprint)
    native.put(native.passenger + 0xB4, passenger_kind)
    native.put(native.passenger + 2, 42)
    native.put(native.carrier + 2, 101)
    native.put(native.passenger + 0x130, 0x01000000)
    native.put(native.carrier + 0x68, start_x << 16)
    native.put(native.carrier + 0x6C, start_y << 16)
    native.put(native.carrier + 0x70, start_z << 16)
    native.put(native.carrier + 0x74, (start_x >> 4) | ((start_z >> 4) << 16))
    native.put(native.carrier + 0xA4, 0)
    native.put(native.carrier + 0xB0, 0)
    native.word(native.carrier + 0x7E, 0)
    native.put(native.nav + 0x0C, start_x << 16)
    native.put(native.nav + 0x10, start_y << 16)
    native.put(native.nav + 0x14, start_z << 16)
    native.word(native.nav + 0x24, 0)
    native.put(native.mover + 0x20, 0)
    native.put(native.mover + 0x30, 0)
    native.word(native.mover + 0x36, 2)

    native.put(native.mission + 0x22, target_x << 16)
    native.put(native.mission + 0x26, target_y << 16)
    native.put(native.mission + 0x2A, target_z << 16)
    native.put(native.kind + 0x126, footprint)
    native.put(passenger_kind + 0x126,
               passenger_profile[0] | (passenger_profile[1] << 16))
    native.put(passenger_kind + 0x192,
               passenger_profile[2] | ((-10000 & 0xFFFF) << 16))
    uc.mem_write(passenger_kind + 0x23C,
                 bytes((passenger_profile[3], passenger_profile[4])))
    native.byte(passenger_kind + 0x24A, 1)
    passenger_footprint = passenger_profile[0] | (passenger_profile[1] << 16)
    native.put(native.passenger + 0x78, passenger_footprint)
    native.put(HEAP + 0x3D0000 + 42 * 312 + 0x78, passenger_footprint)
    native.put(HEAP + 0xB8000 + 0x20, 0)

    sectors, stride = write_native_map(native, map_data)
    sx, sz = start_x >> 7, start_z >> 7
    sector = sectors + (sz * stride + sx) * 10
    native.put(native.carrier + 0xA4, sector)
    native.put(sector + 6, native.carrier)
    native.put(native.game + 0x19F30, 1)
    native.put(native.game + 0x174C8, 101)
    native.put(native.game + 0x174CC, 102)

    # An actual direct native placement query proves the site/type/feature
    # records are coherent before the dispatcher reaches the same call.
    packed_site = site[0] | (site[1] << 16)
    p.hooks.pop(0x507D10, None)
    placement_result, error = p.call(0x507D10,
        (passenger_kind, 42, packed_site, 1, 0))
    if error:
        raise RuntimeError(("native 0x507d10 Cairbray preflight", error))
    if placement_result != 1:
        raise AssertionError(("native 0x507d10 rejected authored Araarch site",
                              placement_result, site, target_y))

    # Record calls/results from the real placement routine when the live unload
    # handler reaches it. The code itself stays unhooked.
    placement_events = []
    return_handles = {}
    def placement_entry(machine, address, _size, _data):
        esp = machine.reg_read(UC_X86_REG_ESP)
        return_address = struct.unpack("<I", machine.mem_read(esp, 4))[0]
        args = struct.unpack("<5I", machine.mem_read(esp + 4, 20))
        event = {"return_address": return_address, "args": args, "result": None}
        placement_events.append(event)
        if return_address not in return_handles:
            def placement_return(machine2, _address, _size2, _data2):
                pending = next((item for item in reversed(placement_events)
                                if item["return_address"] == return_address and
                                item["result"] is None), None)
                if pending is not None:
                    pending["result"] = machine2.reg_read(UC_X86_REG_EAX)
            return_handles[return_address] = p.uc.hook_add(
                UC_HOOK_CODE, placement_return, begin=return_address, end=return_address)

    p.uc.hook_add(UC_HOOK_CODE, placement_entry, begin=0x507D10, end=0x507D10)
    return native, sectors, stride, placement_events


def compare(binary, root, hpitool, steps):
    root = Path(root)
    map_data = parse_tnt(cat(hpitool, root, "maps.hpi", f"Maps/{MAP}.tnt"))
    width, height, sea, heights, feature_ids, feature_count = map_data
    if (width, height, sea) != (192, 224, 58):
        raise AssertionError(("Cairbray TNT dimensions/sea level", width, height, sea))
    start = START
    site = SITE
    exit_water = EXIT
    start_height = heights[start[1] * width + start[0]]
    site_height = heights[site[1] * width + site[0]]
    exit_height = heights[exit_water[1] * width + exit_water[0]]
    if not (start_height < sea and exit_height < sea and site_height >= sea):
        raise AssertionError(("preflight corridor is no longer water/land/water",
                              start_height, site_height, exit_height, sea))
    araarch_features = [feature_ids[z * width + x]
                        for z in range(site[1], site[1] + 2)
                        for x in range(site[0], site[0] + 2)]
    if araarch_features != [0xFFFF] * 4:
        raise AssertionError(("Araarch footprint is no longer feature-free", araarch_features))

    profile = carrier_profile(hpitool, root, "zonroc", False)
    moveinfo = cat(hpitool, root, "data.hpi", "gamedata/moveinfo.tdf").decode("latin1")
    araarch = cat(hpitool, root, "data.hpi", "units/araarch.fbi").decode("latin1")
    passenger_profile = movement_profile(moveinfo, araarch)
    header, world_rows = world_trace(binary, root, start, site, steps)
    native, sectors, stride, placement_events = setup_native(
        root, hpitool, map_data, profile, passenger_profile, start, site, world_rows)

    # Native movement timing is initialized from the selected FBI profile just
    # as 4dc800's unit parser does; this is O2 Standard only.
    maximum = fixed(profile["maxvelocity"])
    multiplier = max(fixed(profile["watermultiplier"]),
                     fixed(profile["roadmultiplier"]), 65536)
    best_speed = (multiplier * maximum) >> 16
    scan_interval = 255 if best_speed <= 0 else max(1, min(255, (8 << 16) // best_speed))
    native.byte(native.kind + 0x249, scan_interval)

    rows = []
    scan_deadlines = []
    relinks = 0
    previous_sector = native.get(native.carrier + 0xA4)
    controller = 0
    terminal = False
    settings = HEAP + 0x1E0000
    options = settings + 0x1000
    native.put(0x62D558, settings)
    native.put(settings, options)
    native.put(settings + 8, options)
    native.p.uc.mem_write(options, bytes(0x100))
    sector_end = sectors + stride * ((height + 7) // 8) * 10
    for tick in range(1, min(steps, len(world_rows)) + 1):
        native.dispatch(tick)
        native.put(0x62D558, settings)
        native.put(settings, options)
        native.put(settings + 8, options)
        _, error = native.p.call(0x4DC800, (native.carrier,), ecx=native.mover)
        if error:
            raise RuntimeError(("retail 0x4dc800 live Cairbray mover", tick, error))
        controller_now = native.get(native.nav + 4)
        if controller_now and not controller:
            controller = controller_now
            if controller < sector_end and controller + 0x100 > sectors:
                raise AssertionError(("controller overlaps native sector table",
                                      hex(controller), hex(sectors), hex(sector_end)))
        actual = native.get(native.carrier + 0xA4)
        if actual != previous_sector:
            previous_sector = actual
            relinks += 1
        rows.append(native_row(native, tick))
        deadline = native.get(native.mover + 0x30)
        if not scan_deadlines or deadline != scan_deadlines[-1]:
            scan_deadlines.append(deadline)
        row = rows[-1]
        if not row[16] and not row[28] and not row[29] and row[31]:
            terminal = True
            break

    for index, (world, retail) in enumerate(zip(world_rows, rows), 1):
        if world == retail:
            continue
        differences = [i for i, (a, b) in enumerate(zip(world, retail)) if a != b]
        if (differences == [20] and world[17] == retail[17] == 0 and
                world[20] == 0x500 and retail[20] == 0):
            continue
        if (differences == [19] and world[16] == retail[16] == 0 and
                world[19] == 0xFFFFFFFF and retail[19] == 0):
            # World serializes an inactive scan deadline as its sentinel;
            # the native mission record's unused slot remains zero.
            continue
        raise AssertionError({"tick": index, "fields": differences,
                              "World": world, "retail": retail})
    if not terminal:
        raise AssertionError(("retail joined unload did not reach release/PARK/retirement",
                              steps, rows[-1] if rows else None,
                              placement_events[-4:]))
    if not placement_events or any(event["result"] != 1 for event in placement_events):
        raise AssertionError(("live retail placement calls/results", placement_events[-8:]))
    if not controller or len(scan_deadlines) < 2:
        raise AssertionError(("live native terrain scan/controller not exercised",
                              controller, scan_deadlines))

    print(header)
    print(f"PASS: {len(rows)} paired O2 Standard ZONROC/Araarch ticks match on {MAP}; "
          f"Cairbray TNT {width}x{height} sea={sea}, corridor heights "
          f"water/start {start}={start_height}, land/site {site}={site_height}, "
          f"water/exit {exit_water}={exit_height}; native 0x50e740 built {stride}x"
          f"{(height + 7) // 8} verified sectors, 0x4dc800 made {relinks} sector "
          f"relinks and advanced {len(scan_deadlines)-1} live scan deadlines; "
          f"native 0x507d10 accepted live placement {placement_events[-1]['args']} "
          f"=> {placement_events[-1]['result']}; release/PARK/retirement ticks "
          f"{next(row[0] for row in rows if not row[28] and not row[29])}/"
          f"{next(row[0] for row in rows if row[31])}/"
          f"{next(row[0] for row in rows if not row[16])}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/transport_test")
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default="build/hpitool")
    parser.add_argument("--steps", type=int, default=1200)
    args = parser.parse_args()
    compare(args.binary, args.retail_root, args.hpitool,
            max(1, min(args.steps, 3000)))


if __name__ == "__main__":
    main()
