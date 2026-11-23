#!/usr/bin/env python3
"""Probe native sea-unload retry cancellation and a replacement destination.

KINGDOMS executes the ground-unload handler, dispatcher, navigator arrival
callback, and real 0x4d6ad0 mission remover. Landing feasibility, movement to
the authored circle goal, effects, cargo unlinking, and PARK installation are
controlled host boundaries. The existing transport_test binary checks the
corresponding World Stop/reissue regression; this does not compare sea routing.
"""
import argparse
import struct
import subprocess

from emu import HEAP
from probe_transport_surface_unload_callbacks import SurfaceUnload


def check_native():
    # Native 0x507d10 gets the final allow-moving flag at argument +16. A
    # strict failure followed by a relaxed pass represents a moving blocker,
    # which sends GROUND_UNLOAD to its bounded retry stage.
    native = SurfaceUnload(
        placement_result=lambda args, _call: int(args[4] == 1),
        real_mission_removal=True)

    approach = native.dispatch(1)
    assert approach[0:3] == (1, 1, 0x701), approach
    assert native.requests == [1], native.requests

    events, controller_detached, arrival = native.arrive_inside_goal()
    assert events & 0x100 and controller_detached, (hex(events), controller_detached)
    assert arrival[0:3] == (1, 2, 1), arrival

    blocked = native.dispatch(3)
    assert blocked[0:3] == (1, 3, 1), blocked
    assert len(native.placementCalls) == 2, native.placementCalls
    assert native.placementCalls[0][4] == 0 and native.placementCalls[1][4] == 1, \
        native.placementCalls
    assert native.get(native.carrier + 0xAC) == native.passenger
    assert native.get(native.passenger + 0xA8) == native.carrier

    # Supply the real retained-target reference chain expected by the remover.
    native.put(native.carrier + 0xC4, native.mission + 0x12)
    native.put(native.passenger + 0xC4, native.mission + 0x12)
    _, error = native.p.call(0x4D6AD0, (native.carrier, native.mission))
    if error:
        raise RuntimeError(error)
    assert native.get(native.carrier + 0x60) == 0
    assert native.get(native.carrier + 0xAC) == native.passenger
    assert native.get(native.passenger + 0xA8) == native.carrier

    # Issue a fresh mission while retaining the same attached passenger. This
    # point differs from the canceled goal and is intentionally in transfer
    # range, so the probe does not claim native coastal route parity.
    native.mission = HEAP + 0xB0000
    native.p.uc.mem_write(native.mission, bytes(0x100))
    destination = (440 * 65536 + 16384, 0, 600 * 65536 + 32768)
    native.put(native.mission + 0x0E, native.carrier)
    native.put(native.mission + 0x16, native.passenger)
    native.p.uc.mem_write(native.mission + 0x22, struct.pack('<3i', *destination))
    native.byte(native.mission + 4, 1)
    native.byte(native.mission + 5, 1)
    native.put(native.mission + 0x0A, 0xFFFFFFFF)
    native.put(native.carrier + 0x60, native.mission)
    native.put(native.carrier + 0xC4, native.mission + 0x12)
    native.put(native.passenger + 0xC4, native.mission + 0x12)
    native.placementResult = 1
    native.placementCalls.clear()

    reissued = native.dispatch(4)
    assert reissued[0:3] == (1, 2, 1), reissued
    assert native.get(native.passenger + 0xA8) == native.carrier

    released_at = None
    retired_at = None
    for tick in range(5, 32):
        row = native.dispatch(tick)
        if native.get(native.carrier + 0xAC) == 0 and released_at is None:
            released_at = tick
            position = tuple(struct.unpack('<3i',
                native.p.uc.mem_read(native.passenger + 0x68, 12)))
            assert position == destination, (position, destination)
        if row[0] == 0:
            retired_at = tick
            break
        if released_at is None:
            assert native.get(native.passenger + 0xA8) == native.carrier
        else:
            assert native.get(native.passenger + 0xA8) == 0

    assert released_at == 20, released_at
    assert retired_at == 21, retired_at
    assert native.get(native.carrier + 0xAC) == 0
    assert native.get(native.passenger + 0xA8) == 0
    assert native.parked
    print('PASS: native sea GROUND_UNLOAD reaches retry stage 3 after strict-fail/relaxed-pass placement, then real 0x4d6ad0 removes it while retaining both cargo links')
    print(f'PASS: a new native sea-unload mission releases that same passenger at {destination} on tick {released_at} and retires on tick {retired_at}')


def check_same_trip_retry():
    # Keep the original mission and destination after the stage-3 delay, then
    # make its exact landing point clear. This exercises the real GROUND_UNLOAD
    # dispatcher retry, rather than removing the mission and creating another.
    native = SurfaceUnload(placement_result=lambda args, _call: int(args[4] == 1))
    mission = native.mission
    destination = bytes(native.p.uc.mem_read(mission + 0x22, 12))

    assert native.dispatch(1)[0:3] == (1, 1, 0x701)
    events, detached, arrived = native.arrive_inside_goal()
    assert events & 0x100 and detached and arrived[0:3] == (1, 2, 1)
    blocked = native.dispatch(3)
    assert blocked[0:3] == (1, 3, 1), blocked
    assert native.p.uc.mem_read(mission + 0x22, 12) == destination

    # The stage-3 handler sleeps ten ticks, returns to stage 1, and retries the
    # same in-range site. Its dispatcher-owned mission bytes and cargo links must
    # stay live until transfer completes.
    native.placementResult = 1
    released_at = retired_at = None
    for tick in range(4, 40):
        row = native.dispatch(tick)
        assert native.get(native.carrier + 0x60) == mission or not row[0]
        assert native.p.uc.mem_read(mission + 0x22, 12) == destination
        if native.get(native.carrier + 0xAC) == 0 and released_at is None:
            released_at = tick
            assert native.get(native.passenger + 0xA8) == 0
        elif released_at is None:
            assert native.get(native.carrier + 0xAC) == native.passenger
            assert native.get(native.passenger + 0xA8) == native.carrier
        if not row[0]:
            retired_at = tick
            break

    assert released_at == 30, released_at
    assert retired_at == 31, retired_at
    assert native.parked
    assert native.get(native.carrier + 0xAC) == 0
    assert native.get(native.passenger + 0xA8) == 0
    print(f'PASS: the original native sea-unload mission resumes after its ten-tick blocked retry, releases at tick {released_at}, and retires at tick {retired_at}')


def check_same_trip_out_of_range_retry():
    # Leave transfer range during the blocked retry. Retail must replace the
    # old circle controller and request a route back to the original site.
    # Physical path travel is paired separately by
    # check_surface_unload_retry_route.py from the same remote position/site.
    native = SurfaceUnload(placement_result=lambda args, _call: int(args[4] == 1))
    mission = native.mission
    destination = bytes(native.p.uc.mem_read(mission + 0x22, 12))

    assert native.dispatch(1)[0:3] == (1, 1, 0x701)
    events, detached, arrived = native.arrive_inside_goal()
    assert events & 0x100 and detached and arrived[0:3] == (1, 2, 1)
    assert native.dispatch(3)[0:3] == (1, 3, 1)
    old_controller = native._controllerAllocations[0]
    assert native.get(old_controller) == 0x5F28D8

    # Move to a distant point on the connected water lane used by the paired
    # physical route fixture. It remains well beyond range of the saved site.
    native.put(native.carrier + 0x68, 400 << 16)
    native.put(native.carrier + 0x70, 160 << 16)
    native.placementResult = 1
    for tick in range(4, 15):
        row = native.dispatch(tick)

    assert row[0:2] == (1, 1), row
    assert native.get(native.carrier + 0x60) == mission
    assert native.p.uc.mem_read(mission + 0x22, 12) == destination
    assert native.get(native.carrier + 0xAC) == native.passenger
    assert native.requests == [1, 0, 1], native.requests
    assert len(native._controllerAllocations) == 2, native._controllerAllocations
    new_controller = native._controllerAllocations[1]
    assert new_controller != old_controller
    assert native.get(old_controller) == 0x5F28A4, hex(native.get(old_controller))
    retry_goal = native.controller_goal()
    assert retry_goal == (0x5F28D8, (31, 31), 116), retry_goal
    assert native.get(native.nav + 4) == new_controller
    retry_position = tuple(struct.unpack('<3i', native.p.uc.mem_read(native.carrier + 0x68, 12)))
    assert retry_position == (400 << 16, 0, 160 << 16), retry_position

    # Deliver native route arrival, then let the original mission transfer and
    # release its passenger at the unchanged selected point.
    events, detached, resumed = native.arrive_inside_goal(tick=15)
    assert events & 0x100 and detached and resumed[0:3] == (1, 2, 1), resumed
    released_at = retired_at = None
    for tick in range(16, 40):
        row = native.dispatch(tick)
        assert native.get(native.carrier + 0x60) == mission or not row[0]
        assert native.p.uc.mem_read(mission + 0x22, 12) == destination
        if native.get(native.carrier + 0xAC) == 0 and released_at is None:
            released_at = tick
            assert native.get(native.passenger + 0xA8) == 0
            position = tuple(struct.unpack('<3i',
                native.p.uc.mem_read(native.passenger + 0x68, 12)))
            assert position == (500 << 16, 0, 500 << 16), position
        if not row[0]:
            retired_at = tick
            break

    assert released_at == 31, released_at
    assert retired_at == 32, retired_at
    assert native.parked
    print('PASS: an out-of-range retry preserves its original sea-unload mission and landing point, retires the old circle controller, requests a fresh route, and installs a new exact-site circle controller')
    print(f'PASS: after controlled arrival on that new controller, the same native mission releases cargo at tick {released_at} and retires at tick {retired_at}')
    return {
        'position': retry_position,
        'destination': tuple(struct.unpack('<3i', destination)),
        'controller_goal': retry_goal,
    }


def check_world(binary):
    result = subprocess.run([binary], text=True, capture_output=True, check=True)
    expected = (
        'blocked unload reaches its retry stage before cancellation',
        'Stop removes the blocked retry mission without releasing or losing cargo',
        'a fresh unload succeeds after canceling a blocked retry',
        'blocked retry state and old destination do not leak into the replacement unload',
        'transfer restarts at the same point after the blocker walks away',
        'the retried transfer releases its passenger normally',
        'transport_test: all passed',
    )
    for text in expected:
        assert text in result.stdout, text
    print('PASS: existing World air/surface Stop and replacement-unload regressions pass in transport_test')
    print('PASS: existing World air/surface same-trip blocked-transfer recovery regressions pass in transport_test')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()
    check_native()
    check_same_trip_retry()
    check_same_trip_out_of_range_retry()
    check_world(args.binary)


if __name__ == '__main__':
    main()
