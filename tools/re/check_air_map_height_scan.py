#!/usr/bin/env python3
"""Compare an air-unload flight with native map-built height sectors enabled.

Unlike check_air_unload_heightstep.py, this probe runs retail's actual map
initializer (0x50e740) over cell records, then keeps the mover's local scan
deadline live while 0x4dc800 flies the carrier across the height boundary.
"""
import argparse
import struct
import subprocess

from emu import HEAP, HEAP_SZ
from probe_transport_air_unload_flight import (
    MAP_WIDTH, SECTOR_STRIDE, native_row, prepare_native,
)


def world_trace(binary, steps):
    output = subprocess.run(
        [binary, "--air-unload-heightstep-trace", str(steps)],
        check=True, capture_output=True, text=True,
    ).stdout
    rows = [tuple(map(int, line.split())) for line in output.splitlines()]
    if len(rows) != steps:
        raise AssertionError(("World trace length", len(rows), steps))
    return rows


def random_draws_from_world(rows):
    # The VTOL dispatcher polls rand(6). World deadline deltas expose each
    # draw, allowing the native mission to consume the same mission randomness.
    draws = {}
    previous_deadline = None
    for row in rows:
        tick, deadline = row[0], row[19]
        if row[17] == 1 and deadline and deadline != previous_deadline:
            draw = deadline - tick - 6
            if not 0 <= draw < 6:
                raise AssertionError(("unexpected VTOL poll interval", tick,
                                      deadline, draw))
            draws[tick] = draw
        previous_deadline = deadline
    return draws


def populate_native_map(native):
    """Build the same ridge map using retail's real sector-table initializer."""
    p = native.p
    uc = p.uc
    game = native.game
    width = height = MAP_WIDTH
    cells = native.get(game + 0x19F04)

    # The World fixture has a 100-height plain, then a 220-height ridge from
    # cell 220 eastward. Retail's sector builder reads the per-cell high corner
    # at +5, so construct it from the same four terrain samples as map loading.
    records = bytearray(width * height * 14)

    def terrain(x):
        return 220 if x >= 220 else 100

    for z in range(height):
        for x in range(width):
            corners = (terrain(x), terrain(min(x + 1, width - 1)))
            index = (z * width + x) * 14
            records[index + 4] = terrain(x)
            records[index + 5] = max(corners)
            records[index + 6] = min(corners)
            struct.pack_into("<H", records, index + 8, 0xFFFF)
    uc.mem_write(cells, bytes(records))

    # 0x50e740 converts map dimensions to sector counts from the world-space
    # dimensions at +0x19e88/+0x19e8c, and reads cell counts at +0x19e98/+0x19e9c.
    native.put(game + 0x19E88, width * 16)
    native.put(game + 0x19E8C, height * 16)
    native.put(game + 0x19E98, width)
    native.put(game + 0x19E9C, height)
    native.byte(game + 0x19EF8, 40)

    # NativeAirUnload's allocator is reserved for mission controllers. Swap
    # just its existing emulation hook while the one-time map initializer runs,
    # using memory beyond both the game fixture and the 256x256 cell plane.
    p.hooks = dict(p.hooks)
    cursor = [HEAP + 0x310000]

    def allocate(uc, argp):
        size = struct.unpack("<I", uc.mem_read(argp, 4))[0]
        address = cursor[0]
        end = address + max(size, 1)
        if address < HEAP + 0x300000 or end > HEAP + HEAP_SZ:
            raise RuntimeError(("map allocation escaped scratch arena", address,
                                size, hex(HEAP + HEAP_SZ)))
        uc.mem_write(address, bytes(size))
        cursor[0] = (end + 15) & ~15
        return 0, address  # malloc is cdecl; the caller pops its size argument

    p.hooks[0x4EB9E0] = allocate
    _, error = p.call(0x50E740)
    if error:
        raise RuntimeError(("retail map-height initialization", error))

    sectors = native.get(game + 0x19F18)
    stride = native.get(game + 0x19F1C)
    rows = (height + 7) // 8
    if stride != (width + 7) // 8 or rows != SECTOR_STRIDE:
        raise AssertionError(("native sector dimensions", stride, rows,
                              SECTOR_STRIDE))

    # Verify the grid built by native 0x50e740 against the map input, including
    # its sea-level floor and both axes of the 3x3 max dilation.
    raw = []
    for sector_z in range(rows):
        for sector_x in range(stride):
            max_height = 40
            for z in range(sector_z * 8, min(height, sector_z * 8 + 8)):
                for x in range(sector_x * 8, min(width, sector_x * 8 + 8)):
                    max_height = max(max_height, terrain(x),
                                     terrain(min(x + 1, width - 1)))
            raw.append(max_height)
    for sector_z in range(rows):
        for sector_x in range(stride):
            index = sector_z * stride + sector_x
            expected_dilated = max(
                raw[neighbor_z * stride + neighbor_x]
                for neighbor_z in range(max(0, sector_z - 1),
                                        min(rows, sector_z + 2))
                for neighbor_x in range(max(0, sector_x - 1),
                                        min(stride, sector_x + 2)))
            record = sectors + index * 10
            actual = tuple(uc.mem_read(record, 2))
            expected_raw = raw[index]
            if actual != (expected_raw, expected_dilated):
                raise AssertionError(("native map sector", sector_x, sector_z,
                                      actual, (expected_raw, expected_dilated)))
    return sectors, stride


def compare(binary, steps):
    world = world_trace(binary, steps)
    draws = random_draws_from_world(world)
    native = prepare_native(
        random_value=lambda bound, tick: draws.get(tick, 0) if bound == 6 else 0)
    p = native.p
    uc = p.uc
    sectors, stride = populate_native_map(native)

    # 4dc800's local scan timer is intentionally live. A 70-tick cell interval
    # provides repeated scans during the flight; the initial zero deadline
    # makes the first movement pass exercise the real scanner immediately.
    settings = HEAP + 0x1E0000
    options = settings + 0x1000
    native.put(0x62D558, settings)
    native.put(settings, options)
    native.put(settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    native.byte(native.kind + 0x249, 70)
    native.put(native.mover + 0x30, 0)

    rows = []
    scan_deadlines = []
    relinks = 0
    previous_pointer = native.get(native.carrier + 0xA4)
    for tick in range(1, steps + 1):
        native.dispatch(tick)
        # Dispatch temporarily swaps the global context pointer; the mover
        # scan must see the same enabled options state on every tick.
        native.put(0x62D558, settings)
        native.put(settings, options)
        native.put(settings + 8, options)
        _, error = p.call(0x4DC800, (native.carrier,), ecx=native.mover)
        if error:
            raise RuntimeError(("native map-backed air mover", tick, error))

        position = struct.unpack("<3i", uc.mem_read(native.carrier + 0x68, 12))
        sector_x, sector_z = position[0] >> 23, position[2] >> 23
        expected_pointer = sectors + (sector_z * stride + sector_x) * 10
        actual_pointer = native.get(native.carrier + 0xA4)
        if actual_pointer != expected_pointer:
            raise AssertionError(("native height-sector relink", tick,
                                  (sector_x, sector_z), hex(actual_pointer),
                                  hex(expected_pointer)))
        if actual_pointer != previous_pointer:
            relinks += 1
            previous_pointer = actual_pointer
        rows.append(native_row(native, tick))
        deadline = native.get(native.mover + 0x30)
        if not scan_deadlines or deadline != scan_deadlines[-1]:
            scan_deadlines.append(deadline)

    if not relinks:
        raise AssertionError("native mover did not relink a map height sector")
    if len(scan_deadlines) < 3:
        raise AssertionError(("native local terrain scan did not repeat",
                              scan_deadlines))
    for tick, (world_row, native_row_value) in enumerate(zip(world, rows), 1):
        if world_row != native_row_value:
            differences = [i for i, (a, b) in enumerate(zip(world_row,
                                                             native_row_value))
                           if a != b]
            raise AssertionError({"tick": tick, "fields": differences,
                                  "World": world_row, "native": native_row_value})
    print(f"PASS: {steps} World/native VTOL-unload ticks match exactly over a "
          "map-built 100-to-220 ridge; retail 0x50e740 built and verified the "
          f"height sectors, 0x4dc800 relinked {relinks} center sectors, and "
          f"its live scan deadline advanced {len(scan_deadlines) - 1} times "
          f"({scan_deadlines[0]} through {scan_deadlines[-1]})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/transport_test")
    parser.add_argument("--steps", type=int, default=480,
                        help="ticks before the separate arrival-event phase")
    args = parser.parse_args()
    compare(args.binary, max(1, min(args.steps, 486)))


if __name__ == "__main__":
    main()
