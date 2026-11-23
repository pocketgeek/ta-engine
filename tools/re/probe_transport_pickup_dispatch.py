#!/usr/bin/env python3
"""Compare full retail carrier-pickup dispatch with the World's in-range trace.

KINGDOMS.icd executes 4d8450 and both GROUND_PICKUP/VTOL_PICKUP handlers. The
native sleep/deadline routine and event filtering remain real. Controller,
eligibility, effect, attachment, and queue-removal boundaries are controlled.
This isolates the mission clock/dispatcher from movement and candidate search.
"""
import argparse
import struct
import subprocess

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW


def native_trace(binary, flying):
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
    carrier, passenger, kind, mission, game, mover, owner, passenger_mission = (
        HEAP + i * 0x10000 for i in range(8))
    definitions = HEAP + 0x80000
    controller = HEAP + 0x90000

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    def byte(address, value):
        p.uc.mem_write(address, bytes((value & 255,)))

    def string_id(uc, _sp):
        # The handlers ask the mission table for Move_Seek_Pickup. Its id is 1
        # in this controlled table, matching the reciprocal request below.
        byte(uc.reg_read(UC_X86_REG_ECX), 1)
        return 1, 0

    def attach(uc, sp):
        args = struct.unpack('<5I', uc.mem_read(sp, 20))
        assert args == (passenger, carrier, 0xFFFFFFFF, 0, 1), args
        put(passenger + 0xA8, carrier)
        return 5, 0

    def remove_order(uc, sp):
        unit, order = struct.unpack('<2I', uc.mem_read(sp, 8))
        assert (unit, order) == (carrier, mission)
        put(unit + 0x60, get(order + 0x66))
        return 2, 0

    p.hooks.update({
        0x4D4BF0: string_id,
        0x519F50: lambda _uc, _sp: (1, 1),
        0x4D4D40: lambda _uc, _sp: (1, 0),
        0x4D6AD0: remove_order,
        0x4D6B30: lambda _uc, _sp: (2, 0),
        0x4D6A50: lambda _uc, _sp: (2, 0),
        0x4F5DB0: lambda _uc, _sp: (2, 0),
        0x50A9C0: lambda _uc, _sp: (3, 0),
        0x421E10: lambda _uc, _sp: (2, 0),
        0x51B4F0: attach,
        0x4EB9E0: lambda _uc, _sp: (0, controller),
        0x4EBA00: lambda _uc, _sp: (0, 0),
        0x4E3F70: lambda _uc, _sp: (2, controller),
        0x4E40E0: lambda _uc, _sp: (2, controller),
        0x4E4540: lambda _uc, _sp: (1, 0),
        0x416C50: lambda _uc, _sp: (4, 0),
    })
    p.freeze_hooks()

    handler = 0x41A680 if flying else 0x408860
    put(0x62D55C, game)
    put(0x62DB84, definitions)
    put(definitions + 25 + 4, handler)
    put(0x64186C, 100)
    put(game + 0x19F30, 1)
    put(game + 0x19EF8, 0)
    put(game + 0x174C8, 101)
    put(game + 0x174CC, 102)

    put(carrier + 0xB4, kind)
    put(carrier + 8, mover)
    put(carrier + 0xA4, 0)  # exercise the authoritative, non-local carrier path
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
    byte(kind + 0x24B, 0)  # do not construct an idle mission after completion
    put(mover + 0x20, 0)
    put(owner, 1)
    byte(owner + 0xEA, 1)

    byte(mission + 4, 1)
    byte(mission + 5, 0)
    put(mission + 0x0A, 0xFFFFFFFF)
    put(mission + 0x0E, carrier)
    put(mission + 0x16, passenger)
    put(carrier + 0x60, mission)

    out = []
    for tick in range(1, 19):
        put(game + 0x19F44, tick)
        _, error = p.call(0x4D8450, (carrier,))
        if error:
            raise RuntimeError(error)
        active = get(carrier + 0x60) == mission
        out.append((int(flying), tick,
                    p.uc.mem_read(mission + 5, 1)[0] if active else -1,
                    get(mission + 0x52) if active else 0,
                    get(mission + 0x4E) if active else 0,
                    int(get(passenger + 0xA8) == carrier)))
    assert get(carrier + 0x60) == 0
    assert get(passenger + 0xA8) == carrier
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()

    native = native_trace(args.binary, False) + native_trace(args.binary, True)
    port_text = subprocess.run([args.binary, '--pickup-timeline'], text=True,
                               capture_output=True, check=True).stdout
    port = [tuple(map(int, line.split())) for line in port_text.splitlines()]
    if port != native:
        mismatch = next((i for i, pair in enumerate(zip(port, native))
                         if pair[0] != pair[1]), min(len(port), len(native)))
        raise AssertionError({
            'row': mismatch,
            'port': port[mismatch:mismatch + 1],
            'native': native[mismatch:mismatch + 1],
        })
    print('PASS: 36 native air/sea pickup dispatcher ticks match the World from initialization through attachment/removal')


if __name__ == '__main__':
    main()
