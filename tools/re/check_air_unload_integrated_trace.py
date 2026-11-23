#!/usr/bin/env python3
"""Pair a distant air-unload mission and real flight mover over many ticks.

Retail executes the actual VTOL_UNLOAD dispatcher, flight-point controller,
0x4dc800 mover, and navigator callbacks. The World side uses the corresponding
transport test fixture. Terrain is a shared flat plane and the native random
sleep draws are aligned to the World fixture's recorded mission deadlines.
Landing, effects, and passenger-reference operations remain host boundary
sinks, as in probe_transport_air_unload_callbacks.py.
"""
import argparse
import struct
import subprocess

from emu import HEAP
from probe_transport_air_unload_callbacks import NativeAirUnload


CORE_FIELDS = 16
WORLD_META = 16


def put_words(uc, address, *values):
    uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                      *(value & 0xffffffff for value in values)))


def put_dword(retail, address, value):
    retail.put(address, value)


def read_i32(uc, address, count):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, count * 4))


def world_trace(binary, steps):
    result = subprocess.run([binary, '--air-unload-flight-trace', str(steps)],
                            check=True, capture_output=True, text=True)
    rows = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    if len(rows) != steps or any(len(row) != 32 for row in rows):
        raise AssertionError(f'World trace shape: {len(rows)} rows, lengths '
                             f'{sorted({len(row) for row in rows})}')
    return rows


def retail_trace(world, steps):
    def synced_random(bound, tick):
        if bound != 6:
            return 0
        deadline = world[tick - 1][CORE_FIELDS + 3]
        delay = deadline - tick
        if not 6 <= delay <= 11:
            raise AssertionError(f'tick {tick}: unexpected World poll delay {delay}')
        return delay - 6

    retail = NativeAirUnload(random_value=synced_random)
    uc = retail.p.uc
    put = retail.put
    word = retail.word
    owner = retail.get(retail.carrier + 0xA4)

    # The production fixture uses a 256x256 World map of 100-unit terrain.
    # The native body-sector plane is 128-pixel sectors, with extra room for
    # the flight unit's step-out leg.
    settings, options, sectors = (HEAP + offset for offset in
                                  (0xB0000, 0xB1000, 0xB2000))
    sector_stride = 64
    put_dword(retail, 0x62D558, settings)
    put_dword(retail, settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    put_dword(retail, retail.game + 0x19E98, 512)
    put_dword(retail, retail.game + 0x19E9C, 512)
    put_dword(retail, retail.game + 0x19F18, sectors)
    put_dword(retail, retail.game + 0x19F1C, sector_stride)
    sector_data = bytearray(sector_stride * sector_stride * 10)
    sector_data[1::10] = bytes([100]) * (sector_stride * sector_stride)
    uc.mem_write(sectors, bytes(sector_data))
    sector_x = 3000 >> 7
    sector_z = 3000 >> 7
    current_sector = sectors + (sector_z * sector_stride + sector_x) * 10
    put_dword(retail, current_sector + 6, retail.carrier)

    # Match the World fixture's 16.16 starting state and FBI movement values.
    put_dword(retail, retail.mission + 0x22, 4000 << 16)
    put_dword(retail, retail.mission + 0x26, 100 << 16)
    put_dword(retail, retail.mission + 0x2A, 3000 << 16)
    put_words(uc, retail.carrier + 0x68, 3000 << 16, 100 << 16, 3000 << 16)
    word(retail.carrier + 0x7E, 0)  # World fixture's explicit retail heading 0
    put_dword(retail, retail.carrier + 0x12B, 120000)
    put_dword(retail, retail.kind + 0x166, 60000)  # lateral limit
    put_dword(retail, retail.kind + 0x16A, 20000)  # acceleration
    put_dword(retail, retail.kind + 0x16E, 65536)
    put_dword(retail, retail.kind + 0x172, 65536)
    word(retail.kind + 0x18E, 10000)
    word(retail.kind + 0x23A, 100)
    word(retail.kind + 0x23E, 150)
    put_dword(retail, retail.kind + 0x260, 0x800)
    put_dword(retail, retail.kind + 0x264, 0x200)
    word(retail.kind + 0x126, 1)
    word(retail.kind + 0x128, 1)

    put_dword(retail, retail.carrier + 0xB0, 0)
    put_dword(retail, retail.mover, retail.nav)
    put_dword(retail, retail.mover + 0x20, 0)
    put_dword(retail, retail.mover + 0x30, 0x7FFFFFFF)
    put_words(uc, retail.mover + 8, 0, 0, 0)
    uc.mem_write(retail.mover + 0x36, struct.pack('<H', 2))
    put_words(uc, retail.nav + 0x0C, 3000 << 16, 100 << 16, 3000 << 16)

    rows = []
    for tick in range(1, steps + 1):
        # Retail's transport mission reads the owning player through +0xa4;
        # the native mover uses that field as its current body-sector record.
        put_dword(retail, retail.carrier + 0xA4, owner)
        retail.dispatch(tick)
        put_dword(retail, retail.carrier + 0xA4, current_sector)
        _, error = retail.p.call(0x4DC800, (retail.carrier,), ecx=retail.mover)
        current_sector = retail.get(retail.carrier + 0xA4)
        put_dword(retail, retail.carrier + 0xA4, owner)
        if error:
            raise RuntimeError({'tick': tick, 'native mover': error})

        position = read_i32(uc, retail.carrier + 0x68, 3)
        heading = struct.unpack('<H', uc.mem_read(retail.carrier + 0x7E, 2))[0]
        speed = struct.unpack('<i', uc.mem_read(retail.mover + 0x20, 4))[0]
        velocity = read_i32(uc, retail.mover + 8, 3)
        navigation = read_i32(uc, retail.nav + 0x0C, 6)
        nav_heading = struct.unpack('<H', uc.mem_read(retail.nav + 0x24, 2))[0]
        active = retail.get(retail.nav + 4)
        if active:
            goal = read_i32(uc, active + 0x26, 3)
            flags = struct.unpack('<H', uc.mem_read(active + 8, 2))[0]
            radius = struct.unpack('<H', uc.mem_read(active + 0xA, 2))[0]
        else:
            goal, flags, radius = (0, 0, 0), 0, 0
        mission_active = retail.get(retail.carrier + 0x60) == retail.mission
        stage = uc.mem_read(retail.mission + 5, 1)[0] if mission_active else 0
        wait = retail.get(retail.mission + 6) if mission_active else 0
        deadline = retail.get(retail.mission + 0xA) if mission_active else 0
        pending = retail.get(retail.mission + 0x6A) if mission_active else 0
        cargo = int(retail.get(retail.carrier + 0xAC) != 0)
        aboard = int(retail.get(retail.passenger + 0xA8) == retail.carrier)
        effect_delta = retail.effects
        rows.append((tick, *position, heading, speed, *velocity, *navigation,
                     nav_heading, int(mission_active), stage, wait, deadline,
                     pending, int(bool(active)), *goal, flags, radius, cargo,
                     aboard, effect_delta, int(retail.parked)))
    return rows


def compare(binary, steps):
    world = world_trace(binary, steps)
    retail = retail_trace(world, steps)
    for tick, (world_row, native_row) in enumerate(zip(world, retail), 1):
        # World stores one paired transport-effect event per beam tick; retail
        # dispatches the site and carrier effects separately.
        effect_count = world_row[CORE_FIELDS + 14]
        world_projection = (
            *world_row[:CORE_FIELDS],
            world_row[CORE_FIELDS], world_row[CORE_FIELDS + 1],
            world_row[CORE_FIELDS + 2], world_row[CORE_FIELDS + 3],
            # World latches controller arrival into mission.pending; retail
            # posts those same 0x500 bits to the unit event word consumed by
            # the next dispatch. Compare the wait state, not its storage slot.
            world_row[CORE_FIELDS + 4] & ~0x500, world_row[CORE_FIELDS + 6],
            *world_row[CORE_FIELDS + 7:CORE_FIELDS + 12],
            world_row[CORE_FIELDS + 12], world_row[CORE_FIELDS + 13],
            effect_count, int(bool(world_row[CORE_FIELDS + 15])),
        )
        if native_row != world_projection:
            raise AssertionError({'tick': tick, 'World': world_projection,
                                  'retail': native_row})
    print(f'PASS: {steps} paired retail/World ticks match through the distant '
          'VTOL unload approach, range handoff, step-out flight, transfer, '
          'passenger release and mission retirement')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/transport_test')
    parser.add_argument('--steps', type=int, default=500)
    args = parser.parse_args()
    compare(args.binary, max(1, min(args.steps, 10000)))


if __name__ == '__main__':
    main()
