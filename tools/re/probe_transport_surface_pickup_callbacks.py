#!/usr/bin/env python3
"""Trace retail surface-pickup navigator callbacks through the dispatcher.

The native circle-goal constructor, 4e5150 arrival callback, 4e4ea0 empty-
route failure callback, mission event method and 4d8450 dispatcher all execute
in KINGDOMS.icd. Only navigator installation, allocation/cleanup, passenger
eligibility, feedback and queue ownership are boundary sinks. Route search and
waypoint motion are not emulated here.
"""
import struct
import subprocess
import sys

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW


class SurfacePickup:
    def __init__(self):
        self.p = Icd()
        self.p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
        (self.carrier, self.passenger, self.kind, self.mission, self.game,
         self.mover, self.owner, self.passenger_mission, self.nav,
         self.nav_vtable) = (HEAP + i * 0x10000 for i in range(10))
        self.definitions = HEAP + 0xA0000
        self.controller = HEAP + 0xB0000
        self.pool = HEAP + 0xD0000
        self.installs = []
        self.removed = []
        self.transfer_effects = 0

        def put(address, value):
            self.p.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

        def get(address):
            return struct.unpack('<I', self.p.uc.mem_read(address, 4))[0]

        def byte(address, value):
            self.p.uc.mem_write(address, bytes((value & 255,)))

        self.put, self.get, self.byte = put, get, byte

        def mission_id(uc, _sp):
            byte(uc.reg_read(UC_X86_REG_ECX), 1)
            return 1, 0

        def install(uc, sp):
            controller = get(sp)
            put(self.nav + 4, controller)
            self.installs.append(controller)
            return 1, 0

        def remove_order(uc, sp):
            unit, order = struct.unpack('<2I', uc.mem_read(sp, 8))
            assert (unit, order) == (self.carrier, self.mission)
            self.removed.append(order)
            put(unit + 0x60, get(order + 0x66))
            return 2, 0

        def allocate(_uc, _sp):
            return 0, self.controller

        def transfer_effect(_uc, _sp):
            self.transfer_effects += 1
            return 2, 0

        self.p.hooks.update({
            0x4D4BF0: mission_id,
            0x519F50: lambda _uc, _sp: (1, 1),
            0x401000: install,       # navigator's virtual SetController
            0x4D6AD0: remove_order,
            0x4D6B30: lambda _uc, _sp: (2, 0),
            0x4D6A50: lambda _uc, _sp: (2, 0),
            0x4F5DB0: lambda _uc, _sp: (2, 0),
            0x535CC0: lambda _uc, sp: (1, 0),  # deterministic Random(n)
            0x50A9C0: lambda _uc, _sp: (3, 0),
            0x421E10: transfer_effect,
            0x4EB9E0: allocate,
            0x4EBA00: lambda _uc, _sp: (0, 0),
            0x416C50: lambda _uc, _sp: (4, 0),
        })
        self.p.freeze_hooks()

        put(0x62D55C, self.game)
        put(0x62DB84, self.definitions)
        put(self.definitions + 25 + 4, 0x408860)  # GROUND_PICKUP
        put(0x64186C, 100)
        put(self.game + 0x19F30, 1)
        put(self.game + 0x19EF8, 0)
        put(self.game + 0x174C8, 101)
        put(self.game + 0x174CC, 102)

        # Unit, mover and ground navigator layout used by 4d4d40/4e5150.
        put(self.carrier + 0xB4, self.kind)
        put(self.carrier + 8, self.mover)
        put(self.carrier + 0xA4, 0)
        put(self.carrier + 0xB8, self.owner)
        put(self.carrier + 0x130, 0x1000000)
        put(self.carrier + 0x78, 1 | (1 << 16))  # one-cell footprint
        put(self.carrier + 0x68, 400 << 16)
        put(self.carrier + 0x70, 400 << 16)
        put(self.carrier + 0x74, 25 | (25 << 16))  # ground-navigation cell X/Z
        put(self.mover, self.nav)
        put(self.nav, self.nav_vtable)
        put(self.nav + 8, self.carrier)
        put(self.nav + 0x10C, 0)
        put(self.nav + 0x110, 0)
        put(self.nav + 0x114, 0)
        put(self.nav_vtable + 4, 0x401000)

        put(self.passenger + 0xB4, self.kind)
        put(self.passenger + 8, self.mover)
        put(self.passenger + 0x130, 0x1000000)
        put(self.passenger + 0x68, 1000 << 16)
        put(self.passenger + 0x6C, 100 << 16)
        put(self.passenger + 0x70, 400 << 16)
        put(self.passenger + 0x60, self.passenger_mission)
        byte(self.passenger_mission + 4, 1)
        put(self.passenger_mission + 0x16, self.carrier)

        put(self.kind + 0x23E, 150)
        put(self.kind + 0x260, 0)
        put(self.kind + 0x14A, 1 << 16)
        byte(self.kind + 0x24B, 0)
        put(self.mover + 0x20, 0)  # passenger has stopped
        put(self.owner, 1)
        byte(self.owner + 0xEA, 1)
        put(self.owner + 0x74, self.pool)
        put(self.owner + 0x78, self.pool - 1)  # no nearby alternate passenger

        byte(self.mission + 4, 1)
        byte(self.mission + 5, 0)
        put(self.mission + 0x0A, 0xFFFFFFFF)
        put(self.mission + 0x0E, self.carrier)
        put(self.mission + 0x16, self.passenger)
        put(self.carrier + 0x60, self.mission)

    def dispatch(self, tick):
        self.put(self.game + 0x19F44, tick)
        _, error = self.p.call(0x4D8450, (self.carrier,))
        if error:
            raise RuntimeError(error)
        active = self.get(self.carrier + 0x60) == self.mission
        return {
            'active': active,
            'stage': self.p.uc.mem_read(self.mission + 5, 1)[0] if active else -1,
            'mask': self.get(self.mission + 6) if active else 0,
            'deadline': self.get(self.mission + 0x0A) if active else 0,
            'events': self.get(self.mission + 0x6A) if active else 0,
            'controller': self.get(self.nav + 4),
            'ticks': self.get(self.mission + 0x52) if active else 0,
            'attempts': self.get(self.mission + 0x4E) if active else 0,
        }


def port_abort(binary, event):
    output = subprocess.run([binary, '--pickup-approach-abort'],
                            input=f'0 1 {event} 0\n', text=True,
                            capture_output=True, check=True).stdout.strip()
    return bool(int(output))


def install_surface_goal(run):
    first = run.dispatch(1)
    assert first['active'] and first['stage'] == 1 and first['deadline'] == 2, first
    second = run.dispatch(2)
    assert second['active'] and second['stage'] == 1 and second['mask'] == 0x709, second
    assert second['controller'] == run.controller and second['controller'] != 0
    assert run.installs == [run.controller]


def check(binary):
    # Circle arrival: 4e5150 calls the real goal acceptance virtual and
    # 4e2470 writes 0x100 to the mission before 4d8450 consumes it.
    run = SurfacePickup()
    install_surface_goal(run)
    run.put(run.carrier + 0x68, 1050 << 16)
    run.put(run.carrier + 0x74, 66 | (25 << 16))
    _, error = run.p.call(0x4E5150, ecx=run.nav)
    assert not error, error
    assert run.get(run.mission + 0x6A) & 0x100, hex(run.get(run.mission + 0x6A))
    arrived = run.dispatch(3)
    assert arrived['active'] and arrived['stage'] == 2, arrived
    assert arrived['mask'] == 1 and arrived['deadline'] == 4, arrived
    assert arrived['events'] & 0x100 == 0
    assert not port_abort(binary, 0x100)
    transfer = run.dispatch(4)
    assert transfer['active'] and transfer['stage'] == 2, transfer
    assert (transfer['mask'], transfer['deadline'], transfer['ticks'],
            transfer['attempts']) == (1, 5, 1, 0), transfer
    port_transfer = subprocess.run([binary, '--pickup-transfer'],
                                   input='0 0 0 0\n', text=True,
                                   capture_output=True, check=True).stdout.strip()
    assert tuple(map(int, port_transfer.split())) == (2, 0, 1, 1, 1), port_transfer
    assert run.transfer_effects == 2  # native transfer emits its two phase effects

    # Empty-route failure: 4e4ea0 checks the same native circle goal and
    # emits 0x200; the real dispatcher consumes it and aborts the stopped
    # out-of-range sea pickup, matching the shared port gate.
    run = SurfacePickup()
    install_surface_goal(run)
    empty_points = HEAP + 0xC0000
    _, error = run.p.call(0x4E4EA0, (empty_points, 0), ecx=run.nav)
    assert not error, error
    assert run.get(run.mission + 0x6A) & 0x200, hex(run.get(run.mission + 0x6A))
    failed = run.dispatch(3)
    assert not failed['active'] and failed['events'] == 0, failed
    assert run.removed == [run.mission]
    assert port_abort(binary, 0x200)

    print('PASS: native sea pickup circle arrival 4e5150 -> 0x100 -> 4d8450 transfer wake and first transfer tick match the port state')
    print('PASS: native empty-route failure 4e4ea0 -> 0x200 -> 4d8450 abort/removal matches the port gate')

    # In-range pickup: compare the whole carrier mission clock, from its
    # one-tick initialization through the fifteen-tick transfer and retirement.
    # Position is held in transfer range; route movement and attachment callbacks
    # are separate boundaries covered by their own probes/World tests.
    run = SurfacePickup()
    run.put(run.passenger + 0x68, 430 << 16)
    run.put(run.passenger + 0x70, 400 << 16)
    native = []
    for tick in range(1, 19):
        row = run.dispatch(tick)
        native.append((0, tick, row['stage'] if row['active'] else -1,
                       row['ticks'] if row['active'] else 0,
                       row['attempts'] if row['active'] else 0))
    world_output = subprocess.run([binary, '--pickup-timeline'],
                                  text=True, capture_output=True, check=True).stdout
    world = [tuple(map(int, line.split()))[:5] for line in world_output.splitlines()
             if line.split() and line.split()[0] == '0']
    assert native == world, next(((i + 1, n, w) for i, (n, w) in
                                  enumerate(zip(native, world)) if n != w),
                                 (len(native), len(world)))
    print('PASS: 18 paired native/World surface-pickup dispatcher ticks match from initialization through transfer wait and mission retirement')


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/transport_test'
    check(binary)


if __name__ == '__main__':
    main()
