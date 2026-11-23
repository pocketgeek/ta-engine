#!/usr/bin/env python3
"""Pair a live native VTOL-unload mission with its flight mover and World.

One native carrier, passenger, mission, navigator, controller, and mover run
continuously from distant unload command through arrival, transfer, release,
and the empty mission tail. The native and World fixtures use the same flat
map, direct circle goal, and held terrain-scan deadline. This intentionally
isolates mission/mover integration; it does not run route search or dynamic
body collision.
"""
import argparse
import struct
import subprocess

from emu import HEAP
from probe_transport_air_unload_callbacks import NativeAirUnload


MAP_WIDTH = 256
SECTOR_STRIDE = (MAP_WIDTH + 7) // 8
START = (3000, 100, 3000)
SITE = (4000, 100, 3000)


def prepare_native(random_value=None):
    native = NativeAirUnload(random_value=random_value)
    p = native.p

    # Match the flat 100-height World fixture and the existing native flight
    # trace's body-sector layout. Keep backing planes outside NativeAirUnload's
    # retained game/unit/controller objects in the emulated heap.
    settings = HEAP + 0x1E0000
    options = settings + 0x1000
    cells = HEAP + 0x220000
    units = HEAP + 0x3D0000
    sectors = HEAP + 0x200000
    native.put(0x62D558, settings)
    native.put(settings + 8, options)
    p.uc.mem_write(options, bytes(0x100))
    native.put(native.game + 0x19E98, MAP_WIDTH)
    native.put(native.game + 0x19E9C, MAP_WIDTH)
    native.put(native.game + 0x19F04, cells)
    native.put(native.game + 0x19EDC, units)
    native.put(native.game + 0x19EC0, 64)
    p.uc.mem_write(cells, bytes(MAP_WIDTH * MAP_WIDTH * 14))
    p.uc.mem_write(units, bytes(0x140 * 64))
    native.put(native.game + 0x19F18, sectors)
    native.put(native.game + 0x19F1C, SECTOR_STRIDE)
    native.put(native.game + 0x19F30, 1)
    for index in range(SECTOR_STRIDE * SECTOR_STRIDE):
        p.uc.mem_write(sectors + index * 10 + 1, b"\x64")

    sx, sz = START[0] >> 7, START[2] >> 7
    current_sector = sectors + (sz * SECTOR_STRIDE + sx) * 10
    native.put(native.carrier + 0xA4, current_sector)
    native.put(current_sector + 6, native.carrier)
    native.put(native.carrier + 0xB0, 0)
    p.uc.mem_write(native.carrier + 0x68,
                   struct.pack("<3i", *(value << 16 for value in START)))
    p.uc.mem_write(native.carrier + 0x74,
                   struct.pack("<hh", START[0] >> 4, START[2] >> 4))
    p.uc.mem_write(native.carrier + 0x78, struct.pack("<hh", 1, 1))
    p.uc.mem_write(native.carrier + 0x7E, struct.pack("<H", 0))
    native.put(native.carrier + 0x12B, 120000)
    native.put(native.carrier + 0x130, 0x01000000)

    native.put(native.mover, native.nav)
    native.put(native.nav + 0x0C, START[0] << 16)
    native.put(native.nav + 0x10, START[1] << 16)
    native.put(native.nav + 0x14, START[2] << 16)
    native.word(native.nav + 0x24, 0)
    native.put(native.mover + 0x20, 0)
    native.put(native.mover + 0x30, 0x7FFFFFFF)  # hold terrain scan past arrival
    native.word(native.mover + 0x36, 2)

    # Same FBI movement profile as airUnloadFlightTraceFixture.
    native.put(native.kind + 0x166, 60000)  # brakerate
    native.put(native.kind + 0x16A, 20000)  # acceleration
    native.put(native.kind + 0x16E, 65536)
    native.put(native.kind + 0x172, 65536)
    native.word(native.kind + 0x18E, 10000)  # turn rate
    p.uc.mem_write(native.kind + 0x23A, struct.pack("<h", 100))

    # VTOL_UNLOAD receives the requested landing site while the native handler
    # constructs the active transportdistance-34 circle controller.
    native.put(native.mission + 0x22, SITE[0] << 16)
    native.put(native.mission + 0x26, SITE[1] << 16)
    native.put(native.mission + 0x2A, SITE[2] << 16)
    return native


def native_row(native, tick):
    p = native.p.uc
    word = lambda address: struct.unpack("<H", p.mem_read(address, 2))[0]
    byte = lambda address: p.mem_read(address, 1)[0]
    get = lambda address: struct.unpack("<I", p.mem_read(address, 4))[0]
    ints = lambda address, count: struct.unpack("<" + "i" * count,
                                                  p.mem_read(address, count * 4))
    active = get(native.carrier + 0x60) == native.mission
    controller = get(native.nav + 4)
    goal = (0, 0, 0)
    flags = radius = 0
    if controller:
        goal = ints(controller + 0x26, 3)
        flags = word(controller + 8)
        radius = word(controller + 0xA)
    passenger_aboard = get(native.passenger + 0xA8) == native.carrier
    cargo = get(native.carrier + 0xAC) != 0
    return (
        tick, *ints(native.carrier + 0x68, 3), word(native.carrier + 0x7E),
        ints(native.mover + 0x20, 1)[0], *ints(native.mover + 8, 3),
        *ints(native.nav + 0x0C, 6), word(native.nav + 0x24),
        int(active), byte(native.mission + 5) if active else 0,
        get(native.mission + 6) if active else 0,
        get(native.mission + 0xA) if active else 0,
        get(native.mission + 0x6A) if active else 0,
        get(native.mission + 0x52) if active else 0,
        int(bool(controller)), *goal, flags, radius,
        int(cargo), int(passenger_aboard), native.effects, int(native.parked),
    )


def compare(binary, steps):
    result = subprocess.run([binary, "--air-unload-flight-trace", str(steps)],
                            check=True, capture_output=True, text=True)
    world_rows = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]

    # Reuse the World fixture's deterministic rand(6) values for each observed
    # poll. A new deadline encodes the draw as (deadline - tick) - 6.
    draws = {}
    previous_deadline = None
    for row in world_rows:
        tick, deadline = row[0], row[19]
        if row[17] == 1 and deadline and deadline != previous_deadline:
            draw = deadline - tick - 6
            if not 0 <= draw < 6:
                raise AssertionError(("unexpected VTOL poll interval", tick, deadline, draw))
            draws[tick] = draw
        previous_deadline = deadline
    native = prepare_native(
        random_value=lambda bound, tick: draws.get(tick, 0) if bound == 6 else 0)
    retail_rows = []
    for tick in range(1, steps + 1):
        native.dispatch(tick)
        _, error = native.p.call(0x4DC800, (native.carrier,), ecx=native.mover)
        if error:
            raise RuntimeError((tick, error))
        retail_rows.append(native_row(native, tick))
        if (not retail_rows[-1][16] and not retail_rows[-1][28] and
                not retail_rows[-1][29] and retail_rows[-1][31]):
            break

    if len(world_rows) < len(retail_rows):
        raise AssertionError(("World trace ended early", len(world_rows), len(retail_rows)))
    if not retail_rows or not world_rows:
        raise AssertionError(("empty unload trace", len(world_rows), len(retail_rows)))
    terminal = retail_rows[-1]
    world_terminal = world_rows[len(retail_rows) - 1]
    for label, row in (("retail", terminal), ("World", world_terminal)):
        if row[16] or row[28] or row[29] or not row[31]:
            raise AssertionError(("unload trace did not reach released-and-parked terminal state",
                                  label, row))
    arrival_phase_pending_samples = 0
    for index, (world, retail) in enumerate(zip(world_rows, retail_rows)):
        if world == retail:
            continue
        differences = [i for i, (a, b) in enumerate(zip(world, retail)) if a != b]
        # World exposes the just-posted 0x500 arrival bits after its movement
        # pass. Retail consumes that same wake in the next native dispatcher
        # before the post-mover snapshot, so its mission pending field is already
        # clear once both fixtures have entered stage 0. All stage/timer/motion,
        # transfer, cargo, goal, and release fields must still match.
        if differences == [20] and world[17] == retail[17] == 0 and world[20] == 0x500 and retail[20] == 0:
            arrival_phase_pending_samples += 1
            continue
        raise AssertionError({"tick": index + 1, "World": world, "retail": retail})
    if arrival_phase_pending_samples != 1:
        raise AssertionError(("unexpected arrival-wake sampling difference",
                              arrival_phase_pending_samples))
    print(f"PASS: {len(retail_rows)} paired native/World VTOL-unload ticks match "
          "from dispatcher-installed circle through mover arrival and cargo release; "
          f"{arrival_phase_pending_samples} post-mover arrival-wake snapshots differ only "
          "because World exposes 0x500 after movement while retail consumes it in dispatcher order")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/transport_test")
    parser.add_argument("--steps", type=int, default=1200)
    args = parser.parse_args()
    compare(args.binary, max(1, min(args.steps, 10000)))


if __name__ == "__main__":
    main()
