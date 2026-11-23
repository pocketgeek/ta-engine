#!/usr/bin/env python3
"""Observe the native active-bit setter and immediate script notifications.

The script invocation boundary is recorded; script execution is covered by the
World command/yard regression. Other unit flags are retained in every case.
"""
import itertools
import struct

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EIP


def main():
    p = Icd()
    unit, owner, script = HEAP, HEAP + 0x1000, HEAP + 0x2000
    p.uc.mem_write(unit + 0xb8, struct.pack('<II', owner, script))
    events = []

    def notify(uc, args):
        name, count, immediate = struct.unpack('<3I', uc.mem_read(args, 12))
        assert uc.reg_read(UC_X86_REG_ECX) == script
        assert (count, immediate) == (0, 1)
        assert name in (0x604a04, 0x6049dc)
        events.append(int(name == 0x604a04))
        return 3, 0

    p.hooks[0x56c5c0] = notify
    count = 0
    for flags, enabled in itertools.product(range(256), (0, 1)):
        p.uc.mem_write(unit + 0x114, bytes([flags]))
        events.clear()
        _, error = p.call(0x51e4d0, (1, enabled), ecx=unit)
        assert error is None, error
        assert p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000
        assert bytes(p.uc.mem_read(unit + 0x114, 1))[0] == (flags & ~1) | enabled
        assert events == ([enabled] if (flags & 1) != enabled else []), (flags, enabled, events)
        count += 1
    print(f'PASS: {count} native active-bit changes and immediate notification edges')


if __name__ == '__main__':
    main()
