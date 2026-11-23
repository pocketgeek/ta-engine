#!/usr/bin/env python3
"""Observe script beam allocation and initialization routing in retail.

Runs the native wrapper and factory; substitutes allocation, geometry
initialization and manager insertion. Does not establish rendering parity.
"""
import random
import struct
from unicorn.x86_const import UC_X86_REG_ECX
from emu import Icd, HEAP

p = Icd()
game, manager, beam, first, second = [HEAP + i * 0x10000 for i in range(5)]
calls = []
allocated = True

def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

def alloc(uc, sp):
    calls.append(('allocate', struct.unpack('<I', uc.mem_read(sp, 4))[0]))
    uc.mem_write(beam, bytes(68))
    return 1, beam if allocated else 0

def initialize(uc, sp):
    assert uc.reg_read(UC_X86_REG_ECX) == beam
    a, b, mode, duration = struct.unpack('<4I', uc.mem_read(sp, 16))
    calls.append(('initialize', bytes(uc.mem_read(a, 12)),
                  bytes(uc.mem_read(b, 12)), mode, duration))
    assert struct.unpack('<I', uc.mem_read(beam, 4))[0] == 0x5f2e80
    assert bytes(uc.mem_read(beam + 0x10, 12)) == bytes(12)
    return 4, 0

def insert(uc, sp):
    assert uc.reg_read(UC_X86_REG_ECX) == manager
    calls.append(('insert', *struct.unpack('<2I', uc.mem_read(sp, 8))))
    return 2, 0

p.hooks.update({0x502820: alloc, 0x504420: initialize, 0x5022f0: insert})
p.freeze_hooks()
put(0x62d55c, game)
put(game + 0x19e5c, manager)
rng = random.Random(0x502580)
for case in range(1024):
    a, b = rng.randbytes(12), rng.randbytes(12)
    p.uc.mem_write(first, a)
    p.uc.mem_write(second, b)
    duration = 6 + case % 2
    allocated = case % 3 != 0
    calls.clear()
    _, error = p.call(0x502a70, (first, second, 1, duration, 7))
    assert not error, error
    expected = [('allocate', 68)]
    if allocated:
        expected += [('initialize', a, b, 1, duration), ('insert', beam, 7)]
    assert calls == expected, (case, calls, expected)
print('PASS: 1024 script beam factory cases preserve both endpoints, mode, duration and manager priority; allocation failure creates nothing')
