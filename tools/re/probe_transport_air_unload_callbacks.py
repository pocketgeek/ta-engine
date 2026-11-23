#!/usr/bin/env python3
"""Pair a native air-unload callback/dispatcher trace with World.

KINGDOMS executes 0x41ae20, 0x4d8450, the 0x4e40e0 flight-point controller,
the real flight navigator vtable, and 0x524af0. Only native heap allocation,
landing feasibility, effects, cargo-list mutation, and PARK installation are
host boundary sinks. The callback advances the navigator to its real native
goal before the next dispatcher tick, matching the controlled arrival in the
World fixture. Flight route integration is deliberately held at this boundary.
"""
import argparse
import struct
import subprocess

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW


class NativeAirUnload:
    def __init__(self, random_value=None, placement_result=1):
        self.p = Icd()
        self.p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
        (self.carrier, self.passenger, self.kind, self.mission, self.game,
         self.mover, self.nav, self.owner, self.local_owner, self.definitions,
         self.controller) = (HEAP + i * 0x10000 for i in range(11))
        self.controllerAllocations = []
        self.randomValue = random_value
        self.placementResult = placement_result
        self.effects = 0
        self.parked = False

        def put(address, value):
            self.p.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

        def get(address):
            return struct.unpack('<I', self.p.uc.mem_read(address, 4))[0]

        def word(address, value):
            self.p.uc.mem_write(address, struct.pack('<H', value & 0xFFFF))

        def byte(address, value):
            self.p.uc.mem_write(address, bytes((value & 255,)))

        self.put, self.get, self.word, self.byte = put, get, word, byte

        def allocate(_uc, _sp):
            address = self.controller + len(self.controllerAllocations) * 0x1000
            if address + 0x100 > HEAP + 0x400000:
                raise RuntimeError("native air-unload controller fixture exhausted its heap")
            self.controllerAllocations.append(address)
            self.p.uc.mem_write(address, bytes(0x100))
            return 0, address

        def bind_passenger(uc, sp):
            # Native mission +0x12 embeds a retained-unit reference; preserve
            # the passenger selected by this one-unit cargo fixture.
            ref = uc.reg_read(UC_X86_REG_ECX)
            put(ref + 4, get(sp))
            return 1, ref

        def placement(uc, sp):
            relaxed = struct.unpack('<5I', uc.mem_read(sp, 20))[4]
            result = self.placementResult(relaxed) if callable(self.placementResult) \
                else self.placementResult
            return 5, result  # controlled native landing-feasibility boundary

        def effect(_uc, _sp):
            self.effects += 1
            return 2, 0

        def detach(_uc, _sp):
            self.p.uc.mem_write(self.passenger + 0x68,
                                bytes(self.p.uc.mem_read(self.mission + 0x22, 12)))
            put(self.passenger + 0xA8, 0)
            put(self.carrier + 0xAC, 0)
            return 5, 0

        def position(uc, sp):
            out = get(sp)
            uc.mem_write(out, bytes(uc.mem_read(self.carrier + 0x68, 12)))
            return 4, 0

        def park(_uc, _sp):
            self.parked = True
            return 11, 0

        def random(_uc, sp):
            bound = get(sp)
            value = self.randomValue(bound, get(self.game + 0x19F44)) \
                if self.randomValue else 0
            return 1, value

        self.p.hooks.update({
            0x4EB9E0: allocate,
            0x5199F0: bind_passenger,
            0x507D10: placement,
            0x421E10: effect,
            0x51B480: position,
            0x51B4F0: detach,
            0x4D78A0: park,
            0x519EF0: lambda _uc, _sp: (0, 0),
            0x535CC0: random,
            0x4D4BF0: lambda _uc, _sp: (1, 0),
            # free is cdecl here; these native callers pop the pointer after
            # returning, so the emulator hook must not consume the argument.
            0x4EBA00: lambda _uc, _sp: (0, 0),
            0x4D6DA0: lambda _uc, _sp: (0, 0),
            0x56C640: lambda _uc, _sp: (8, 0),
            0x50A9C0: lambda _uc, _sp: (3, 0),
            0x4F5DB0: lambda _uc, _sp: (2, 0),
        })
        self.p.freeze_hooks()

        put(0x62D55C, self.game)
        put(0x62DB84, self.definitions)
        put(self.game + 0x19F30, self.local_owner)
        put(self.game + 0x174C8, 101)
        put(self.game + 0x174CC, 102)
        put(self.definitions + 25 + 4, 0x41AE20)  # VTOL_UNLOAD
        put(self.game + 0x19F44, 0)

        # Native owner is a player object; keep it distinct from the local
        # player so 0x41ae20 enters its regular unload mission path.
        put(self.owner, 1)
        byte(self.owner + 1, 2)
        byte(self.owner + 0xEA, 1)
        put(self.carrier + 0xA4, self.owner)
        put(self.carrier + 0xB8, self.owner)
        put(self.carrier + 0xB4, self.kind)
        put(self.carrier + 8, self.mover)
        put(self.carrier + 0xAC, self.passenger)
        put(self.carrier + 0x130, 0x1000000)
        put(self.carrier + 0x68, 3000 << 16)
        put(self.carrier + 0x6C, 200 << 16)
        put(self.carrier + 0x70, 3000 << 16)
        word(self.carrier + 0x7E, 32768)  # retail heading corresponding to port's default
        byte(self.carrier + 0x12A, 0)
        word(self.kind + 0x23E, 150)
        put(self.kind + 0x264, 0x200)
        put(self.kind + 0x260, 0x800)
        word(self.kind + 0x126, 1)
        word(self.kind + 0x128, 1)

        put(self.passenger + 2, 42)
        put(self.passenger + 0xB4, self.kind)
        put(self.passenger + 0xA8, self.carrier)
        put(self.passenger + 0x130, 0x1000000)

        put(self.mission + 0xE, self.carrier)
        put(self.mission + 0x16, self.passenger)
        put(self.mission + 0x22, 3000 << 16)
        put(self.mission + 0x26, 100 << 16)
        put(self.mission + 0x2A, 3000 << 16)
        byte(self.mission, 1)
        byte(self.mission + 4, 1)
        byte(self.mission + 5, 1)
        put(self.mission + 0xA, 0xFFFFFFFF)
        put(self.carrier + 0x60, self.mission)

        # Use the retail flight navigator and point-controller vtables. The
        # callback below runs their actual methods, including 4e2470's event
        # write and 4e4de0's 0x400 controller-removal notification.
        put(self.mover, self.nav)
        put(self.nav, 0x5F34D4)
        put(self.nav + 8, self.carrier)
        put(self.nav + 0xC, 3000 << 16)
        put(self.nav + 0x10, 200 << 16)
        put(self.nav + 0x14, 3000 << 16)

    def callback_arrival(self):
        controller = self.get(self.nav + 4)
        assert controller
        point = tuple(struct.unpack('<3i', self.p.uc.mem_read(controller + 0x26, 12)))
        self.p.uc.mem_write(self.carrier + 0x68, struct.pack('<3i', *point))
        self.p.uc.mem_write(self.nav + 0xC, struct.pack('<3i', *point))
        _, error = self.p.call(0x524AF0, ecx=self.nav)
        if error:
            raise RuntimeError(error)
        pending = self.get(self.mission + 0x6A)
        assert pending & 0x500 == 0x500, hex(pending)
        assert self.get(self.nav + 4) == 0
        return point

    def dispatch(self, tick):
        self.put(self.game + 0x19F44, tick)
        self.effects = 0
        _, error = self.p.call(0x4D8450, (self.carrier,))
        if error:
            raise RuntimeError(error)
        active = self.get(self.carrier + 0x60) == self.mission
        goal_active = self.get(self.nav + 4) != 0
        goal = (0, 0, 0)
        flags = radius = 0
        if goal_active:
            controller = self.get(self.nav + 4)
            goal = tuple(struct.unpack('<3i', self.p.uc.mem_read(controller + 0x26, 12)))
            flags = struct.unpack('<H', self.p.uc.mem_read(controller + 8, 2))[0]
            radius = struct.unpack('<H', self.p.uc.mem_read(controller + 0xA, 2))[0]
        passenger_aboard = self.get(self.passenger + 0xA8) == self.carrier
        cargo = self.get(self.carrier + 0xAC) != 0
        return (tick,
                self.p.uc.mem_read(self.mission + 5, 1)[0] if active else 0,
                self.get(self.mission + 6) if active else 0,
                self.get(self.mission + 0xA) if active else 0,
                self.get(self.mission + 0x6A) if active else 0,
                self.get(self.mission + 0x52) if active else 0,
                int(goal_active), *goal, flags, radius,
                int(cargo), int(passenger_aboard), self.effects, int(self.parked))


def compare(binary):
    native = NativeAirUnload()
    retail_rows = []
    for tick in range(1, 25):
        if tick == 2:
            native.callback_arrival()
        retail_rows.append(native.dispatch(tick))
        if retail_rows[-1][12] == 0 and retail_rows[-1][13] == 0:
            break
    assert len(retail_rows) == 17, len(retail_rows)
    assert retail_rows[0][7:12] == (3000 << 16, 200 << 16, 4100 << 16, 0x30, 16), retail_rows[0]
    assert retail_rows[1][4] & 0x500 == 0x500 and retail_rows[1][6] == 0, retail_rows[1]
    assert retail_rows[1][14] == 2, retail_rows[1]  # the two phase effects
    assert retail_rows[-1][12:16] == (0, 0, 0, 1), retail_rows[-1]

    result = subprocess.run([binary, '--air-unload-world-timeline'],
                            text=True, capture_output=True, check=True)
    world_rows = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    assert len(world_rows) == len(retail_rows), (len(world_rows), len(retail_rows), result.stdout)
    # The C++ fixture exposes the same 16 fields as the native trace.
    for index, (world, retail) in enumerate(zip(world_rows, retail_rows)):
        if world != retail:
            raise AssertionError({'tick': index + 1, 'World': world, 'retail': retail})
    world_end, retail_end = world_rows[-1], retail_rows[-1]
    assert world_end[12:16] == retail_end[12:16] == (0, 0, 0, 1), (world_end, retail_end)
    assert world_end[1:7] == retail_end[1:7] == (0, 1, 18, 0x500, 15, 0), (world_end, retail_end)
    print('PASS: 17 paired native/World air-unload dispatcher ticks match, including native 524af0 arrival/removal callback, controller goal, transfer effects, passenger release, and the one-tick empty mission tail')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()
    compare(args.binary)


if __name__ == '__main__':
    main()
