#!/usr/bin/env python3
"""Observe native hardware-effect frame stride from reported memory.

Executes 4bd169..4bd19b, the budget decision inside hardware-bank loading.
The surrounding asset upload/copy paths and resulting pixels are not tested.
"""
import random
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ECX

p = Icd()
game, settings, frame = HEAP, HEAP + 0x10000, STACK + 0x8000
p.uc.mem_write(0x62d55c, struct.pack('<I', game))
p.uc.mem_write(game + 8, struct.pack('<I', settings))
p.uc.reg_write(UC_X86_REG_EBP, frame)
rng = random.Random(0x4bd169)
cases = [-2147483648, -1, 0, 1, 64*1048576, 65*1048576-1,
         65*1048576, 66*1048576, 2147483647]
cases += [rng.randrange(-2147483648, 2147483648) for _ in range(4096)]
for memory in cases:
    p.uc.mem_write(settings + 0x5ee, struct.pack('<i', memory))
    p.uc.emu_start(0x4bd169, 0x4bd19b)
    stride = struct.unpack('<I', p.uc.mem_read(frame + 8, 4))[0]
    assert stride == (1 if memory >= 65*1048576 else 2), (memory, stride)
    assert p.uc.reg_read(UC_X86_REG_ECX) == stride
print(f'PASS: {len(cases)} native memory decisions select every frame at >=65 MiB; otherwise every second frame, including failure sentinel -1')
