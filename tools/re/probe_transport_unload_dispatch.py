#!/usr/bin/env python3
"""Compare the native sea-unload handler under its real mission dispatcher.

The handler, event filtering, deadlines, and stage dispatch run in KINGDOMS.icd.
Only navigator installation and queue removal are host sinks. The resulting
tick trace is compared with the compiled RetailMissionState dispatcher and
retailTransportUnloadApproach helper.
"""
import argparse
import struct
import subprocess

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def native_trace(binary, rows):
    p = Icd()
    carrier, owner, kind, definitions, game, mission, mover, passenger = (
        HEAP + i * 0x10000 for i in range(8))

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    def byte(address, value):
        p.uc.mem_write(address, bytes((value & 255,)))

    requests = 0

    def install_approach(uc, sp):
        nonlocal requests
        point, radius = struct.unpack('<2I', uc.mem_read(sp, 8))
        assert list(struct.unpack('<3i', uc.mem_read(point, 12))) == [
            500 << 16, 0, 500 << 16]
        assert radius == 116
        requests += 1
        return 2, 0

    def bind_carrier(uc, sp):
        ref = uc.reg_read(UC_X86_REG_ECX)
        put(ref + 4, get(sp))
        return 1, ref

    def remove_order(uc, sp):
        unit, order = struct.unpack('<2I', uc.mem_read(sp, 8))
        assert (unit, order) == (carrier, mission)
        put(unit + 0x60, get(order + 0x66))
        return 2, 0

    p.hooks.update({
        0x4d4da0: install_approach,
        0x5199f0: bind_carrier,
        0x4d6ad0: remove_order,
        0x4f5db0: lambda _uc, _sp: (2, 0),
    })
    p.freeze_hooks()

    put(0x62d55c, game)
    put(0x62db84, definitions)
    put(game + 0x19f30, 1)
    put(definitions + 25 + 4, 0x408d50)  # GROUND_UNLOAD
    put(carrier + 0xb4, kind)
    put(carrier + 0xb8, owner)
    put(carrier + 8, mover)
    put(carrier + 0xac, passenger)
    put(carrier + 0x130, 0x1000000)
    put(carrier + 0x68, 100 << 16)
    put(carrier + 0x6c, 0)
    put(carrier + 0x70, 200 << 16)
    put(kind + 0x23e, 150)
    put(kind + 0x264, 0x200)
    put(owner, 1)
    byte(owner + 0xea, 1)
    byte(kind + 0x24b, 0)
    put(passenger + 0x130, 0x1000000)
    put(passenger + 0xa8, carrier)
    put(passenger + 0xb4, kind)
    p.uc.mem_write(mission, bytes(0x100))
    byte(mission + 4, 1)
    byte(mission + 5, 1)  # World::unloadAt starts at the post-init approach stage.
    put(mission + 0xa, 0xffffffff)
    put(mission + 0xe, carrier)
    put(mission + 0x16, passenger)
    put(mission + 0x22, 500 << 16)
    put(mission + 0x26, 0)
    put(mission + 0x2a, 500 << 16)
    put(carrier + 0x60, mission)

    out = []
    previous_requests = 0
    for tick, raised, in_range, has_mover in rows:
        put(game + 0x19f44, tick)
        put(carrier + 0xd0, get(carrier + 0xd0) | raised)
        put(carrier + 8, mover if has_mover else 0)
        if in_range:
            put(carrier + 0x68, 450 << 16)
            put(carrier + 0x70, 500 << 16)
        else:
            put(carrier + 0x68, 100 << 16)
            put(carrier + 0x70, 200 << 16)
        result, error = p.call(0x4d8450, (carrier,))
        if error:
            raise RuntimeError(error)
        out.append((tick, requests - previous_requests,
                    int(bool(get(carrier + 0x60))),
                    p.uc.mem_read(mission + 5, 1)[0], get(mission + 6),
                    get(mission + 10), get(mission + 0x6a), get(mission + 0x4e),
                    get(carrier + 0xd0)))
        previous_requests = requests
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    args = parser.parse_args()
    scenarios = [
        [
            (0, 0, 0, 1),
            (6, 0x800, 0, 1),  # Unsubscribed movement events must remain queued.
            (14, 0, 0, 1),
            (15, 0, 0, 1),  # The native 15-tick poll retries the exact goal.
            (16, 0x100, 0, 1),  # Arrival outside the transfer radius retries.
            (30, 0, 0, 1),
            (31, 0x200, 0, 1),  # Failure and poll timeout are both delivered.
        ],
        [(0, 0, 0, 0)],  # No mover aborts without installing a route.
    ]
    for rows in scenarios:
        native = native_trace(args.binary, rows)
        request = ''.join(' '.join(map(str, row)) + '\n' for row in rows)
        port = subprocess.run([args.binary, '--unload-approach-timeline'],
                              input=request, text=True, capture_output=True,
                              check=True).stdout.splitlines()
        compiled = [tuple(map(int, line.split())) for line in port]
        if compiled != native:
            mismatch = next((i for i, pair in enumerate(zip(compiled, native))
                             if pair[0] != pair[1]), min(len(compiled), len(native)))
            raise AssertionError({
                'row': mismatch,
                'input': rows[mismatch] if mismatch < len(rows) else None,
                'compiled': compiled[mismatch:mismatch + 1],
                'native': native[mismatch:mismatch + 1],
            })
    count = sum(len(rows) for rows in scenarios)
    print(f'PASS: {count} native surface-unload dispatcher ticks match the compiled handler, timer, event, and queue trace')


if __name__ == '__main__':
    main()
