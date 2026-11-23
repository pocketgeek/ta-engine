#!/usr/bin/env python3
"""Exercise full native pickup dispatch across controller wake/failure events.

Both carrier handlers and 4d8450's event/timer dispatch execute in KINGDOMS.icd.
Only the movement-controller, eligibility, RNG, feedback, and queue-removal
boundaries are controlled. The probe covers surface failure abort, flyer
failure recovery, early arrival wakeup, and timer-driven recovery.
"""
import argparse
import struct
import subprocess

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW


class Pickup:
    def __init__(self, flying):
        self.p = Icd()
        self.p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
        (self.carrier, self.passenger, self.kind, self.mission, self.game,
         self.mover, self.owner, self.passenger_mission, self.pool) = (
            HEAP + i * 0x10000 for i in range(9))
        self.definitions = HEAP + 0xA0000
        self.controller = HEAP + 0xB0000
        self.flying = flying
        self.approaches = []

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

        def ground_approach(uc, sp):
            point, radius = struct.unpack('<2I', uc.mem_read(sp, 8))
            xyz = struct.unpack('<3i', uc.mem_read(point, 12))
            self.approaches.append(('surface', xyz, radius))
            return 2, 0

        def flight_approach(uc, sp):
            own_mission, target = struct.unpack('<2I', uc.mem_read(sp, 8))
            self.approaches.append(('air', own_mission, target))
            return 2, self.controller

        def flight_radius(uc, sp):
            radius = struct.unpack('<I', uc.mem_read(sp, 4))[0]
            self.approaches.append(('radius', radius))
            return 1, 0

        def remove_order(uc, sp):
            unit, order = struct.unpack('<2I', uc.mem_read(sp, 8))
            assert (unit, order) == (self.carrier, self.mission)
            put(unit + 0x60, get(order + 0x66))
            return 2, 0

        self.p.hooks.update({
            0x4D4BF0: mission_id,
            0x519F50: lambda _uc, _sp: (1, 1),
            0x4D4DA0: ground_approach,
            0x4E3F70: flight_approach,
            0x4E4540: flight_radius,
            0x4D4D40: lambda _uc, _sp: (1, 0),
            0x535CC0: lambda _uc, _sp: (1, 0),  # deterministic random(6) == 0
            0x4D6AD0: remove_order,
            0x4D6B30: lambda _uc, _sp: (2, 0),
            0x4D6A50: lambda _uc, _sp: (2, 0),
            0x4F5DB0: lambda _uc, _sp: (2, 0),
            0x4EB9E0: lambda _uc, _sp: (0, self.controller),
            0x4EBA00: lambda _uc, _sp: (0, 0),
            0x416C50: lambda _uc, _sp: (4, 0),
        })
        self.p.freeze_hooks()

        handler = 0x41A680 if flying else 0x408860
        put(0x62D55C, self.game)
        put(0x62DB84, self.definitions)
        put(self.definitions + 25 + 4, handler)
        put(0x64186C, 100)
        put(self.game + 0x19F30, 1)
        put(self.game + 0x19EF8, 0)

        put(self.carrier + 0xB4, self.kind)
        put(self.carrier + 8, self.mover)
        put(self.carrier + 0xA4, 0)
        put(self.carrier + 0xB8, self.owner)
        put(self.carrier + 0x130, 0x1000000)
        put(self.carrier + 0x68, 400 << 16)
        put(self.carrier + 0x70, 400 << 16)

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
        put(self.kind + 0x260, 0x800)
        put(self.kind + 0x14A, 1 << 16)
        byte(self.kind + 0x24B, 0)
        put(self.mover + 0x20, 0)
        put(self.owner, 1)
        byte(self.owner + 0xEA, 1)
        put(self.owner + 0x74, self.pool)
        put(self.owner + 0x78, self.pool - 1)  # no alternate in-range requests

        byte(self.mission + 4, 1)
        byte(self.mission + 5, 0)
        put(self.mission + 0x0A, 0xFFFFFFFF)
        put(self.mission + 0x0E, self.carrier)
        put(self.mission + 0x16, self.passenger)
        put(self.carrier + 0x60, self.mission)

    def dispatch(self, tick, unit_events=0):
        self.put(self.game + 0x19F44, tick)
        self.put(self.carrier + 0xD0, self.get(self.carrier + 0xD0) | unit_events)
        self.approaches.clear()
        _, error = self.p.call(0x4D8450, (self.carrier,))
        if error:
            raise RuntimeError(error)
        active = self.get(self.carrier + 0x60) == self.mission
        return {
            'active': active,
            'stage': self.p.uc.mem_read(self.mission + 5, 1)[0] if active else -1,
            'mask': self.get(self.mission + 6) if active else 0,
            'deadline': self.get(self.mission + 0xA) if active else 0,
            'events': self.get(self.carrier + 0xD0),
            'approaches': list(self.approaches),
        }


def port_wait(binary, flying):
    result = subprocess.run([binary, '--pickup-approach-wait'],
                            input=f'{int(flying)} 0\n', text=True,
                            capture_output=True, check=True)
    mask, delay, bound = map(int, result.stdout.split())
    return mask, delay - 100, bound


def check_interrupted_resumption(flying):
    """Insert/remove a short child through retail's queue and run the dispatcher."""
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
    (carrier, passenger, kind, parent, game, mover, owner, passenger_mission,
     child) = (HEAP + i * 0x10000 for i in range(9))
    definitions = HEAP + 0xA0000
    controller = HEAP + 0xB0000
    removed = []

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    def byte(address, value):
        p.uc.mem_write(address, bytes((value & 255,)))

    def init_ref(uc, _sp):
        address = uc.reg_read(UC_X86_REG_ECX)
        uc.mem_write(address, bytes(16))
        return 2, address

    def bind_ref(uc, sp):
        address = uc.reg_read(UC_X86_REG_ECX)
        put(address + 4, get(sp))
        return 1, address

    def mission_id(uc, _sp):
        byte(uc.reg_read(UC_X86_REG_ECX), 1)
        return 1, 0

    def remove_child(uc, sp):
        unit, order = struct.unpack('<2I', uc.mem_read(sp, 8))
        assert (unit, order) == (carrier, child)
        removed.append(order)
        put(unit + 0x60, get(order + 0x66))
        return 2, 0

    p.hooks.update({
        0x519990: init_ref,
        0x5199F0: bind_ref,
        0x4D4BF0: mission_id,
        0x519F50: lambda _uc, _sp: (1, 1),
        0x4D4D40: lambda _uc, _sp: (1, 0),
        0x4D6AD0: remove_child,
        0x4D6B30: lambda _uc, _sp: (2, 0),
        0x4D6A50: lambda _uc, _sp: (2, 0),
        0x4F5DB0: lambda _uc, _sp: (2, 0),
        0x4EB9E0: lambda _uc, _sp: (0, controller),
        0x4EBA00: lambda _uc, _sp: (0, 0),
        0x4E3F70: lambda _uc, _sp: (2, controller),
        0x4E40E0: lambda _uc, _sp: (2, controller),
        0x4E4540: lambda _uc, _sp: (1, 0),
        0x416C50: lambda _uc, _sp: (4, 0),
    })
    p.freeze_hooks()

    put(0x62D55C, game)
    put(0x62DB84, definitions)
    put(definitions + 25 + 4, 0x41A680 if flying else 0x408860)
    # 4d6a40 is the executable's empty mission handler and returns 5.
    put(definitions + 50 + 4, 0x4D6A40)
    put(0x64186C, 100)
    put(game + 0x19F30, 1)
    put(game + 0x19EF8, 0)

    put(carrier + 0xB4, kind)
    put(carrier + 8, mover)
    put(carrier + 0xA4, 0)
    put(carrier + 0xB8, owner)
    put(carrier + 0x130, 0x1000000)
    put(carrier + 0x68, 400 << 16)
    put(carrier + 0x70, 400 << 16)
    put(passenger + 0xB4, kind)
    put(passenger + 8, mover)
    put(passenger + 0x130, 0x1000000)
    put(passenger + 0x68, 430 << 16)
    put(passenger + 0x6C, 100 << 16)
    put(passenger + 0x70, 400 << 16)
    put(passenger + 0x60, passenger_mission)
    byte(passenger_mission + 4, 1)
    put(passenger_mission + 0x16, carrier)
    put(kind + 0x23E, 150)
    put(kind + 0x260, 0x800)
    put(kind + 0x14A, 1 << 16)
    byte(kind + 0x24B, 0)
    put(mover + 0x20, 0)
    put(owner, 1)
    byte(owner + 0xEA, 1)

    # Simulate an in-progress distant pickup hidden behind a temporary order.
    byte(parent + 4, 1)
    byte(parent + 5, 2)
    put(parent + 6, 0x729 if flying else 0x709)
    put(parent + 0x0A, 777)
    put(parent + 0x0E, carrier)
    put(parent + 0x16, passenger)
    put(carrier + 0x60, parent)
    byte(child + 4, 2)
    put(child + 0x0E, carrier)
    put(child + 0x5A, 0)  # ordinary insertion resets the suspended mission

    _, error = p.call(0x4D7750, (carrier, child))
    assert not error, error
    assert get(carrier + 0x60) == child and get(child + 0x66) == parent
    assert p.uc.mem_read(parent + 5, 1)[0] == 0 and get(parent + 6) == 0

    passenger_before = bytes(p.uc.mem_read(passenger_mission, 0x80))
    put(game + 0x19F44, 100)
    _, error = p.call(0x4D8450, (carrier,))
    assert not error, error
    assert removed == [child]
    assert get(carrier + 0x60) == parent
    # The dispatcher removes the completed child, then runs the resumed pickup
    # in the same tick. Its initialization advances 0 -> 1 and waits one tick.
    assert p.uc.mem_read(parent + 5, 1)[0] == 1
    assert get(parent + 6) == 1 and get(parent + 0x0A) == 101
    assert get(parent + 0x16) == passenger
    assert bytes(p.uc.mem_read(passenger_mission, 0x80)) == passenger_before


def check(binary):
    for flying in (False, True):
        expected_mask = 0x729 if flying else 0x709
        expected_delay = port_wait(binary, flying)
        assert expected_delay == (expected_mask, 6 if flying else 15, 6 if flying else 0)

        run = Pickup(flying)
        first = run.dispatch(1)
        assert first['active'] and first['stage'] == 1
        assert first['mask'] == 1 and first['deadline'] == 2

        approach = run.dispatch(2)
        assert approach['active'] and approach['stage'] == 1
        assert (approach['mask'], approach['deadline'] - 2) == expected_delay[:2]
        if flying:
            assert approach['approaches'] == [
                ('air', run.mission, run.passenger), ('radius', 149)]
        else:
            assert approach['approaches'] == [
                ('surface', (1000 << 16, 100 << 16, 400 << 16), 134)]

        # A real 0x100 navigator arrival outside transfer range wakes the
        # dispatcher early and installs a fresh approach controller.
        arrival = run.dispatch(3, 0x100)
        assert arrival['active'] and arrival['events'] == 0
        assert arrival['mask'] == expected_mask
        assert arrival['deadline'] - 3 == expected_delay[1]
        assert len(arrival['approaches']) == (2 if flying else 1)

        # The same 0x200 route failure aborts a stopped surface pickup, leaving
        # the independently sleeping passenger request untouched. A flyer
        # rebuilds pursuit and keeps polling, as the retail handler does.
        passenger_mission_before = bytes(run.p.uc.mem_read(run.passenger_mission, 0x80))
        failure = run.dispatch(4, 0x200)
        assert bytes(run.p.uc.mem_read(run.passenger_mission, 0x80)) == passenger_mission_before
        if flying:
            assert failure['active'] and failure['events'] == 0
            assert failure['mask'] == expected_mask and failure['deadline'] - 4 == expected_delay[1]
            retry = run.dispatch(10)
            assert retry['active'] and retry['mask'] == expected_mask
            assert retry['deadline'] == 16
            assert len(retry['approaches']) == 2
        else:
            assert not failure['active'] and failure['events'] == 0

    check_interrupted_resumption(False)
    check_interrupted_resumption(True)
    print('PASS: native air/sea dispatcher masks and polling match the port; early-arrival recovery, sea failure abort, and air failure/timer recovery preserve the passenger request')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()
    check(args.binary)


if __name__ == '__main__':
    main()
