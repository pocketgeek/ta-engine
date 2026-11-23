#!/usr/bin/env python3
"""Run retail's real circle-goal arrival callback through sea unload dispatch.

The native sea-unload handler constructs and installs its exact-point circle
controller. The probe supplies a completed route endpoint inside that controller
and then runs the retail navigator arrival callback and mission dispatcher. Route
search and physical waypoint movement remain outside this controlled trace.
"""
import struct
import subprocess
import sys

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX


class SurfaceUnload:
    def __init__(self, placement_result=1, real_mission_removal=False, *,
                 icd=None, game=None, freeze_hooks=True):
        self.p = icd or Icd()
        (self.carrier, self.owner, self.kind, self.definitions, self.game,
         self.mission, self.mover, self.passenger, self.nav,
         self.controller, self.pool) = (HEAP + i * 0x10000 for i in range(11))
        if game is not None:
            self.game = game
        self.requests = []
        self.effects = 0
        self.parked = False
        self.placementResult = placement_result
        self.realMissionRemoval = real_mission_removal
        self.placementCalls = []
        self._insideDispatcher = False
        self._nextController = self.controller
        self._controllerAllocations = []
        self._baseAllocator = self.p.hooks.get(0x4EB9E0)

        def put(address, value):
            self.p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

        def get(address):
            return struct.unpack('<I', self.p.uc.mem_read(address, 4))[0]

        def byte(address, value):
            self.p.uc.mem_write(address, bytes((value & 255,)))

        self.put, self.get, self.byte = put, get, byte

        def allocate(_uc, _sp):
            if not self._insideDispatcher and self._baseAllocator is not None:
                return self._baseAllocator(_uc, _sp)
            # A retry can replace an earlier circle controller. Keep each
            # allocation distinct so destroying the previous object cannot
            # reset the new object's vtable through an aliased fixture pointer.
            controller = self._nextController
            self._nextController += 0x1000
            self._controllerAllocations.append(controller)
            self.p.uc.mem_write(controller, bytes(0x100))
            self.controller = controller
            return 0, controller

        def request(_uc, sp):
            self.requests.append(get(sp))
            return 1, 0

        def bind_reference(uc, sp):
            reference = uc.reg_read(UC_X86_REG_ECX)
            put(reference + 4, get(sp))
            return 1, reference

        def placement(uc, sp):
            args = struct.unpack('<5I', uc.mem_read(sp, 20))
            self.placementCalls.append(args)
            result = self.placementResult(args, len(self.placementCalls)) \
                if callable(self.placementResult) else self.placementResult
            return 5, result

        def effect(_uc, _sp):
            self.effects += 1
            return 2, 0

        def detach(_uc, _sp):
            put(self.passenger + 0xA8, 0)
            put(self.carrier + 0xAC, 0)
            return 5, 0

        def park(_uc, _sp):
            self.parked = True
            return 11, 0

        hooks = {
            0x4EB9E0: allocate,
            0x4E4F50: request,
            0x415F30: lambda _uc, _sp: (1, 0),
            0x4D4BF0: lambda _uc, _sp: (1, 0),
            0x5199F0: bind_reference,
            0x4F5DB0: lambda _uc, _sp: (2, 0),
            0x535CC0: lambda _uc, _sp: (1, 0),
            0x50A9C0: lambda _uc, _sp: (3, 0),
            0x4EBA00: lambda _uc, _sp: (0, 0),
            0x507D10: placement,
            0x421E10: effect,
            0x51B4F0: detach,
            0x4D78A0: park,
        }
        if not self.realMissionRemoval:
            hooks[0x4D6AD0] = lambda _uc, _sp: (2, 0)
        self.p.hooks.update(hooks)
        if freeze_hooks:
            self.p.freeze_hooks()

        put(0x62D55C, self.game)
        put(0x62DB84, self.definitions)
        put(self.definitions + 25 + 4, 0x408D50)  # GROUND_UNLOAD
        put(0x64186C, 100)
        put(self.game + 0x19F30, 1)
        put(self.game + 0x19EF8, 0)
        put(self.game + 0x174C8, 101)
        put(self.game + 0x174CC, 102)

        put(self.carrier + 0xB4, self.kind)
        put(self.carrier + 8, self.mover)
        put(self.carrier + 0xA4, 0)
        put(self.carrier + 0xB8, self.owner)
        put(self.carrier + 0x130, 0x1000000)
        put(self.carrier + 0x78, 1 | (1 << 16))
        put(self.carrier + 0x68, 100 << 16)
        put(self.carrier + 0x6C, 0)
        put(self.carrier + 0x70, 200 << 16)
        put(self.carrier + 0x74, 6 | (12 << 16))
        put(self.mover, self.nav)
        put(self.nav, 0x5F2A24)
        put(self.nav + 8, self.carrier)

        put(self.passenger + 0x130, 0x1000000)
        put(self.passenger + 0xA8, self.carrier)
        put(self.passenger + 0xB4, self.kind)
        put(self.carrier + 0xAC, self.passenger)

        put(self.kind + 0x23E, 150)
        put(self.kind + 0x260, 0)
        put(self.kind + 0x264, 0x200)
        put(self.kind + 0x14A, 1 << 16)
        byte(self.kind + 0x24B, 0)
        put(self.owner, 1)
        byte(self.owner + 0xEA, 1)
        put(self.owner + 0x74, self.pool)
        put(self.owner + 0x78, self.pool - 1)

        # World::unloadAt begins at stage 1 with the exact selected site queued.
        self.p.uc.mem_write(self.mission, bytes(0x100))
        byte(self.mission + 4, 1)
        byte(self.mission + 5, 1)
        put(self.mission + 0x0A, 0xFFFFFFFF)
        put(self.mission + 0x0E, self.carrier)
        put(self.mission + 0x16, self.passenger)
        put(self.mission + 0x22, 500 << 16)
        put(self.mission + 0x26, 0)
        put(self.mission + 0x2A, 500 << 16)
        put(self.carrier + 0x60, self.mission)

    def dispatch(self, tick):
        self.put(self.game + 0x19F44, tick)
        self.effects = 0
        self._insideDispatcher = True
        try:
            _, error = self.p.call(0x4D8450, (self.carrier,))
        finally:
            self._insideDispatcher = False
        if error:
            raise RuntimeError(error)
        active = self.get(self.carrier + 0x60) == self.mission
        controller = self.get(self.nav + 4)
        center = struct.unpack('<hh', self.p.uc.mem_read(controller + 8, 4)) if controller else (0, 0)
        radius = struct.unpack('<i', self.p.uc.mem_read(controller + 0x0C, 4))[0] if controller else 0
        return (int(active), self.p.uc.mem_read(self.mission + 5, 1)[0] if active else 0,
                self.get(self.mission + 6) if active else 0,
                self.get(self.mission + 0x0A) if active else 0,
                self.get(self.mission + 0x6A) if active else 0,
                struct.unpack('<H', self.p.uc.mem_read(self.mission + 0x52, 2))[0] if active else 0,
                int(controller != 0), *center, radius,
                int(self.get(self.carrier + 0xAC) != 0),
                int(self.get(self.passenger + 0xA8) == self.carrier),
                self.effects, int(self.parked))

    def controller_goal(self):
        vtable = self.get(self.controller)
        center = struct.unpack('<hh', self.p.uc.mem_read(self.controller + 8, 4))
        radius = struct.unpack('<i', self.p.uc.mem_read(self.controller + 0x0C, 4))[0]
        return vtable, center, radius

    def arrive_inside_goal(self, tick=2):
        # Native route format contains the start and final footprint cell. The
        # endpoint at (28,31) lies 50 px from the authored (500,500) center,
        # inside the native 150-34 px arrival radius.
        self.put(self.carrier + 0x68, 450 << 16)
        self.put(self.carrier + 0x70, 500 << 16)
        self.put(self.carrier + 0x74, 28 | (31 << 16))
        self.put(self.nav + 0x10C, 2)
        self.put(self.nav + 0x110, 2)
        self.p.uc.mem_write(self.nav + 0x114, b'\x01')
        self.p.uc.mem_write(self.nav + 12, struct.pack('<4h', 6, 12, 28, 31))
        _, error = self.p.call(0x4E5150, ecx=self.nav)
        if error:
            raise RuntimeError(error)
        callback_events = self.get(self.mission + 0x6A)
        detached = self.get(self.nav + 4) == 0
        return callback_events, detached, self.dispatch(tick)


def compare(binary):
    run = SurfaceUnload()
    before = run.dispatch(1)
    assert before[0:3] == (1, 1, 0x701), before
    assert run.get(run.nav + 4) == run.controller and before[6] == 1, before
    assert run.requests == [1], run.requests
    assert run.controller_goal() == (0x5F28D8, (31, 31), 116), run.controller_goal()
    initial_route = struct.unpack('<4h', run.p.uc.mem_read(run.nav + 12, 8))
    assert initial_route == (100, 200, 504, 504), initial_route

    callback_events, detached, arrived = run.arrive_inside_goal()
    # Native 4e5150 delivers the real circle-controller arrival through the
    # mission event path; the unload handler consumes it and enters transfer.
    assert callback_events & 0x100 and detached, (hex(callback_events), detached)
    assert arrived[0:3] == (1, 2, 1), arrived
    assert arrived[4] & 0x100 == 0, arrived

    # Compare the exact approach result and transfer-stage handoff to the shared
    # C++ rule used by World::tickGroundMission.
    output = subprocess.run([binary, '--unload-approach'], input='1 0 0 1 1\n',
                            text=True, capture_output=True, check=True).stdout.strip()
    port = tuple(map(int, output.split()))
    assert port == (1, 1, 1, 1, 1, 0), port
    assert port[0] == 1 and port[2] == 1  # World wrapper consumes completed approach.
    print('PASS: retail circle-goal constructor and arrival callback wake the sea-unload dispatcher; both native and World approach logic report completion for the World wrapper to advance into transfer')

    native_rows = [before, arrived]
    for tick in range(3, 25):
        row = run.dispatch(tick)
        native_rows.append(row)
        if not row[0]:
            break
    result = subprocess.run([binary, '--surface-unload-world-timeline'],
                            text=True, capture_output=True, check=True)
    world_rows = [tuple(map(int, line.split()))[1:] for line in result.stdout.splitlines()]
    assert len(world_rows) == len(native_rows), (len(world_rows), len(native_rows), result.stdout)
    for tick, (world, retail) in enumerate(zip(world_rows, native_rows), 1):
        # The port latches controller arrival/release into mission.pending;
        # retail posts those equivalent 0x500 bits on the unit event word.
        normalized = list(world)
        normalized[4] &= ~0x500
        if tuple(normalized) != retail:
            raise AssertionError({'tick': tick, 'World': tuple(normalized), 'retail': retail})
    assert native_rows[-1][0] == 0 and native_rows[-1][-1] == 1, native_rows[-1]
    print(f'PASS: {len(native_rows)} paired native/World sea-unload dispatcher ticks match, including route arrival, transfer wait, passenger release, PARK, and mission retirement')


if __name__ == '__main__':
    compare(sys.argv[1] if len(sys.argv) > 1 else 'build-o2/transport_test')
