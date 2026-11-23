#!/usr/bin/env python3
"""Observe original gate eligibility and its hidden-command configuration setter.

Only command-token lookup is supplied by the fixture. The command's comparisons,
configuration writes and gate eligibility routine execute in the retail binary.
No production gate adapter is implied by this observation test.
"""
import itertools
import struct

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    p = Icd()
    config, options, debug, player, ai, command, bad = (
        HEAP + offset for offset in range(0, 0x7000, 0x1000))

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value))

    def byte(address, value):
        p.uc.mem_write(address, bytes([value]))

    def call(address, args=(), ecx=None):
        value, error = p.call(address, args, ecx=ecx)
        assert error is None, error
        assert p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000
        return value & 255

    put(0x62d558, config)
    put(config, options)
    put(config + 4, debug)
    put(ai, player)
    count = 0
    for unlocked, auto, enabled, kind, stored in itertools.product(
            (0, 1, 255), (0, 1, 255), (0, 1), (0, 1, 2, 255), (0, 1, 255)):
        byte(debug + 4, unlocked)
        byte(options + 9, auto)
        put(player, enabled)
        byte(player + 0xea, kind)
        byte(ai + 0x1a5, stored)
        expected = 1 if unlocked and auto and enabled and kind == 1 else stored
        assert call(0x409fe0, ecx=ai) == expected
        count += 1

    # Token addresses are taken from the command's comparison sites. Do not
    # embed the original strings or substitute its validation routine.
    tokens = [0x606404, 0x606400, 0x6063f8, 0x6063ec, 0x6063e0]
    p.uc.mem_write(bad, b'fixture-invalid-token\0')
    put(player, 1)
    byte(player + 0xea, 1)
    byte(ai + 0x1a5, 0)
    byte(options + 9, 1)
    cases = 0
    for initial, token_count, wrong in itertools.product((0, 1), (4, 5, 6), range(-1, 5)):
        byte(debug + 4, initial)
        put(command + 0x280, token_count)

        def token(uc, args):
            index = struct.unpack('<I', uc.mem_read(args, 4))[0] - 1
            assert 0 <= index < 5
            return 2, bad if index == wrong else tokens[index]

        p.hooks[0x5389a0] = token
        call(0x425b30, (command,))
        expected = int(token_count == 5 and wrong == -1)
        actual = bytes(p.uc.mem_read(debug + 4, 1))[0]
        assert actual == expected, (initial, token_count, wrong, actual)
        assert call(0x409fe0, ecx=ai) == expected
        cases += 1
    print(f'PASS: {count} native gate eligibility cases and {cases} command/eligibility sequences')


if __name__ == '__main__':
    main()
