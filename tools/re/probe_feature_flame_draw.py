#!/usr/bin/env python3
"""Observe native feature shadow/back-flame/body/front-flame submission.

Runs the final submission block of 4fd430. Final sprite drawing is a sink;
feature lookup, visibility, anchor calculation and GPU pixels are not covered.
"""
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import *

p = Icd()
base, definition = STACK + 0x8000, HEAP
calls = []
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def draw(uc, sp):
    calls.append(struct.unpack('<6I', uc.mem_read(sp, 24)))
    return 6, 0
p.hooks[0x4fac00] = draw
p.freeze_hooks()
count = 0
for mask in range(16):
    shadow, back, body, front = [0x10000+i*0x100 if mask & (1 << i) else 0 for i in range(4)]
    for flags in (0, 4, 8, 12, 0x2000, 0x200c):
        for x, y in ((0, 0), (-123, 456), (32767, -32768)):
            put(definition + 0x13c, flags)
            put(base + 0x10, shadow)
            put(base - 4, back)
            put(base - 8, front)
            for reg, value in ((UC_X86_REG_EBP, base), (UC_X86_REG_ESP, base-0x100),
                               (UC_X86_REG_ESI, definition), (UC_X86_REG_EAX, body),
                               (UC_X86_REG_EBX, x), (UC_X86_REG_EDI, y)):
                p.uc.reg_write(reg, value & 0xffffffff)
            calls.clear()
            p.uc.emu_start(0x4fd9b8, 0x4fda29)
            def row(frame, special):
                return (frame, x & 0xffffffff, y & 0xffffffff, special, 0, 0)
            expected = []
            if shadow and not flags & 0x2000: expected.append(row(shadow, (flags >> 3) & 1))
            if back: expected.append(row(back, 1))
            if body: expected.append(row(body, ((flags >> 2) & ~255) | ((flags >> 2) & 1)))
            if front: expected.append(row(front, 1))
            assert calls == expected, (mask, flags, calls, expected)
            count += 1
print(f'PASS: {count} native feature submissions preserve shadow/back/body/front order, common anchor, optional layers and flame special flag')
