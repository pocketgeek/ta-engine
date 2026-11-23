#!/usr/bin/env python3
"""Pair a blocked native air-unload retry cancellation with World recovery.

Retail executes the air handler, primary dispatcher, and real 0x4d6ad0 queue
removal. Landing feasibility is controlled: the strict check fails while the
relaxed check reports a moving blocker, which advances the mission to retry
stage 3. A fresh native unload is then issued at a different point. The World
side's focused regression is part of transport_test.
"""
import argparse
import struct
import subprocess

from emu import HEAP
from probe_transport_air_unload_callbacks import NativeAirUnload


def check_native():
    native = NativeAirUnload(placement_result=lambda relaxed: int(relaxed == 1))
    first = native.dispatch(1)
    assert first[1] == 2 and first[6] == 1
    native.callback_arrival()
    blocked = native.dispatch(2)
    assert blocked[1] == 3 and blocked[5] == 0, blocked
    assert native.get(native.carrier + 0xAC) == native.passenger
    assert native.get(native.passenger + 0xA8) == native.carrier

    # Make the native retained-reference chain visible to the real queue
    # remover, as in probe_transport_cleanup.py.
    native.put(native.carrier + 0xC4, native.mission + 0x12)
    native.put(native.passenger + 0xC4, native.mission + 0x12)
    _, error = native.p.call(0x4D6AD0, (native.carrier, native.mission))
    if error:
        raise RuntimeError(error)
    assert native.get(native.carrier + 0x60) == 0
    assert native.get(native.carrier + 0xAC) == native.passenger
    assert native.get(native.passenger + 0xA8) == native.carrier

    # Reissue to a distinct exact destination while retaining the same cargo.
    mission = HEAP + 0xB0000
    native.mission = mission
    native.p.uc.mem_write(mission, bytes(0x100))
    native.put(mission + 0x0E, native.carrier)
    native.put(mission + 0x16, native.passenger)
    px, py, pz = struct.unpack('<3i', native.p.uc.mem_read(native.carrier + 0x68, 12))
    destination = (px + 20 * 65536, py, pz + 30 * 65536)
    native.p.uc.mem_write(mission + 0x22, struct.pack('<3i', *destination))
    native.byte(mission + 4, 1)
    native.byte(mission + 5, 1)
    native.put(mission + 0x0A, 0xFFFFFFFF)
    native.put(native.carrier + 0x60, mission)
    native.put(native.carrier + 0xC4, mission + 0x12)
    native.put(native.passenger + 0xC4, mission + 0x12)
    native.placementResult = 1

    reissued = native.dispatch(4)
    assert reissued[1] == 2 and reissued[12:14] == (1, 1), reissued
    native.callback_arrival()
    released = None
    for tick in range(5, 32):
        row = native.dispatch(tick)
        if row[12:14] == (0, 0):
            released = row
            break
        assert native.get(native.passenger + 0xA8) == native.carrier
    assert released is not None, 'replacement unload did not finish'
    position = struct.unpack('<3i', native.p.uc.mem_read(native.passenger + 0x68, 12))
    assert position == destination, (position, destination)
    assert native.get(native.carrier + 0xAC) == 0
    assert native.get(native.passenger + 0xA8) == 0
    return blocked, released, position


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()
    blocked, released, position = check_native()
    world = subprocess.run([args.binary], text=True, capture_output=True, check=True)
    expected_world_checks = (
        'blocked unload reaches its retry stage before cancellation',
        'Stop removes the blocked retry mission without releasing or losing cargo',
        'a fresh unload succeeds after canceling a blocked retry',
        'blocked retry state and old destination do not leak into the replacement unload',
    )
    for expected in expected_world_checks:
        assert expected in world.stdout, expected
    assert 'transport_test: all passed' in world.stdout
    print('PASS: native 0x41ae20 reaches retry stage 3 under the strict/relaxed blocker result; real 0x4d6ad0 removes that mission while preserving carrier/passenger attachment; a fresh mission releases exactly once at its new destination')
    print(f'Native transition: retry stage={blocked[1]}, replacement end tick={released[0]}, passenger position={position}')
    print('PASS: matching World retry-stop-reissue regression passed in transport_test')


if __name__ == '__main__':
    main()
