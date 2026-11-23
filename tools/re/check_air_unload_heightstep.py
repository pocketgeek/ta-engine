#!/usr/bin/env python3
"""Compare VTOL unload flight across a map-backed height-sector boundary.

This short trace uses retail's direct point controller and flight mover with a
100-to-220 height step at world X 3520. It exercises sector relinking and
terrain-relative cruise-height changes, not path search or a long flat flight.
"""
import argparse
import struct
import subprocess

from probe_transport_air_unload_flight import MAP_WIDTH, SECTOR_STRIDE, native_row, prepare_native


def retail_rows(binary, steps):
    world_result = subprocess.run(
        [binary, "--air-unload-heightstep-trace", str(steps)],
        check=True, capture_output=True, text=True)
    world = [tuple(map(int, line.split())) for line in world_result.stdout.splitlines()]
    draws = {}
    previous_deadline = None
    for row in world:
        tick, deadline = row[0], row[19]
        if row[17] == 1 and deadline and deadline != previous_deadline:
            draw = deadline - tick - 6
            if not 0 <= draw < 6:
                raise AssertionError(("unexpected VTOL poll interval", tick, deadline, draw))
            draws[tick] = draw
        previous_deadline = deadline

    native = prepare_native(
        random_value=lambda bound, tick: draws.get(tick, 0) if bound == 6 else 0)
    uc = native.p.uc
    sectors = native.get(native.game + 0x19F18)
    # Native center-sector records are 10 bytes. Retail map setup 0x50e740
    # stores the raw 8x8-cell maximum at +0, then writes its 3x3 sector max
    # dilation at +1; 0x524af0 reads +1. Emulate both planes here instead of
    # putting the undilated map heights directly into the navigator's plane.
    raw_heights = [[220 if (x + 1) * 8 > 220 else 100
                    for x in range(SECTOR_STRIDE)]
                   for _ in range(SECTOR_STRIDE)]
    for z in range(SECTOR_STRIDE):
        for x in range(SECTOR_STRIDE):
            dilated = max(raw_heights[neighbor_z][neighbor_x]
                          for neighbor_z in range(max(0, z - 1),
                                                  min(SECTOR_STRIDE, z + 2))
                          for neighbor_x in range(max(0, x - 1),
                                                  min(SECTOR_STRIDE, x + 2)))
            record = sectors + (z * SECTOR_STRIDE + x) * 10
            uc.mem_write(record, bytes((raw_heights[z][x], dilated)))
    # Keep the explicit terrain-rescan deadline pinned so this isolates the
    # map-backed center-sector height used by cruise-height navigation.
    native.put(native.mover + 0x30, 0x7FFFFFFF)
    rows = []
    sector_transitions = 0
    previous_sector = native.get(native.carrier + 0xA4)
    for tick in range(1, steps + 1):
        native.dispatch(tick)
        _, error = native.p.call(0x4DC800, (native.carrier,), ecx=native.mover)
        if error:
            raise RuntimeError((tick, error))
        position = struct.unpack("<iii", uc.mem_read(native.carrier + 0x68, 12))
        sector_x, sector_z = position[0] >> 23, position[2] >> 23
        expected_sector = sectors + (sector_z * SECTOR_STRIDE + sector_x) * 10
        actual_sector = native.get(native.carrier + 0xA4)
        if actual_sector != expected_sector:
            raise AssertionError(("native center-sector relink", tick,
                                  (sector_x, sector_z),
                                  hex(actual_sector), hex(expected_sector)))
        if actual_sector != previous_sector:
            sector_transitions += 1
            previous_sector = actual_sector
        rows.append(native_row(native, tick))
    return world, rows, sector_transitions


def compare(binary, steps):
    world, retail, sector_transitions = retail_rows(binary, steps)
    if len(world) != steps:
        raise AssertionError(("World trace length", len(world), steps))
    for tick, (world_row, retail_row) in enumerate(zip(world, retail), 1):
        if world_row == retail_row:
            continue
        differences = [i for i, (a, b) in enumerate(zip(world_row, retail_row)) if a != b]
        raise AssertionError({"tick": tick, "fields": differences,
                              "World": world_row, "retail": retail_row})
    if sector_transitions == 0:
        raise AssertionError("native mover did not relink the center sector")
    print(f"PASS: {steps} paired native/World VTOL unload ticks across the "
          "100-to-220 terrain-sector boundary, including XYZ, altitude, "
          f"flight dynamics, direct controller, mission and cargo state; "
          f"native center sector relinked {sector_transitions} times")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/transport_test")
    parser.add_argument("--steps", type=int, default=480,
                        help="ticks to compare before the separate arrival-event phase")
    args = parser.parse_args()
    compare(args.binary, max(1, min(args.steps, 486)))


if __name__ == "__main__":
    main()
