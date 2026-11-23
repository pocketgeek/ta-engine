#!/usr/bin/env python3
"""Observe CRT resource actions in an installed retail executable; no GUI/assets copied."""
import struct
from emu import Icd, HEAP


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def main():
    count = 0
    for opcode in range(16, 21):
        for value in (-50, 0, 25, 16777217, 2147483647):
            machine = Icd()
            game, owner, action, resource = HEAP, HEAP + 0x30000, HEAP + 0x31000, HEAP + 0x32000
            def put(address, fmt, *values):
                machine.uc.mem_write(address, struct.pack(fmt, *values))
            put(0x62d55c, '<I', game)
            put(owner, '<I', 2)
            put(game + 0x2510 + 2 * 0x110, '<I', resource)
            put(action, '<ii', opcode, value)
            put(resource, '<f', 123.25)
            put(resource + 0x14, '<f', 500)
            put(resource + 0x18, '<d', 1000.5)
            _, error = machine.call(0x4cad50, (action, owner + 0x2000), ecx=owner)
            assert not error, error
            stored = struct.unpack('<f', machine.uc.mem_read(resource, 4))[0]
            limit = struct.unpack('<f', machine.uc.mem_read(resource + 0x14, 4))[0]
            produced = struct.unpack('<d', machine.uc.mem_read(resource + 0x18, 8))[0]
            assert stored == (f32(value) if opcode == 17 else
                              f32(123.25 + value) if opcode == 18 else
                              f32(123.25 - value) if opcode == 19 else 123.25)
            assert limit == (f32(value) if opcode == 16 else 0 if opcode == 20 else 500)
            assert produced == (1000.5 + value if opcode == 18 else 1000.5)
            count += 1
    ui, settings = HEAP + 0x40000, HEAP + 0x41000
    put(0x62d558, '<I', ui)
    put(ui + 0x0c, '<I', settings)
    put(action, '<I', 12)
    _, error = machine.call(0x4cad50, (action, owner + 0x2000), ecx=owner)
    assert not error, error
    assert machine.uc.mem_read(settings + 7, 1) == b'\x01'
    print(f'PASS: {count} native resource action cases and Display gameclock flag')


if __name__ == '__main__':
    main()
