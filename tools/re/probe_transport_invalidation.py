#!/usr/bin/env python3
"""Observe native pickup cancellation before stage dispatch for air and sea.

Only the user-feedback sink is replaced. Invalid targets/events must not reach
navigation, transfer effects, or attachment routines.
"""
import struct
from emu import Icd, HEAP

p = Icd()
carrier, passenger, mission = HEAP, HEAP + 0x10000, HEAP + 0x20000
calls = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def chatter(uc, sp):
    unit, message = struct.unpack('<2I', uc.mem_read(sp, 8))
    assert unit == carrier and message == 0x604fac
    calls.append('failure')
    return 2, 0


p.hooks[0x4f5db0] = chatter
p.freeze_hooks()
count = 0
for handler in (0x41a680, 0x408860):
    for stage in range(4):
        for target, flags, events in ((0, 0x1000000, 0),
                (passenger, 0, 0), (passenger, 0x1001000, 0),
                (passenger, 0x1000000, 8)):
            p.uc.mem_write(mission, bytes(0x80))
            p.uc.mem_write(mission + 5, bytes([stage]))
            put(mission + 0x16, target)
            put(mission + 0x4e, 2)
            put(mission + 0x52, 14)
            put(passenger + 0x130, flags)
            before = bytes(p.uc.mem_read(mission, 0x80))
            calls.clear()
            result, error = p.call(handler, (carrier, mission, events))
            assert not error, (hex(handler), stage, error)
            assert result == 8 and calls == ['failure'], (stage, result, calls)
            assert bytes(p.uc.mem_read(mission, 0x80)) == before
            count += 1
print(f'PASS: {count} native air/sea pickup invalidations abort before every stage without mutating mission state')
