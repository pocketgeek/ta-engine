#!/usr/bin/env python3
"""Verify native explosion-class random variant selection, not class loading.

Synthetic class tables contain sentinel pointers. Only the CRT random result is
substituted; native multiplication, division and class indexing all execute.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
table, variants = HEAP, HEAP+0x10000
p.uc.mem_write(0x62d5cc, struct.pack('<I', table))
value = 0
p.hooks[0x5d4444] = lambda uc, sp: (0, value)
p.freeze_hooks()
rng = random.Random(0x492fd0)
for case in range(4096):
    kind = case % 9
    count = 1 + case % 127
    value = (0, 1, 16383, 16384, 32766, 32767)[case % 6] if case < 768 else rng.randrange(32768)
    pointers = [0x72000000+i*16 for i in range(count)]
    p.uc.mem_write(table+kind*12, struct.pack('<3I', 0, count, variants))
    p.uc.mem_write(variants, struct.pack('<'+'I'*count, *pointers))
    result, error = p.call(0x492fd0, (kind,))
    assert not error, error
    assert result == pointers[value*count//32768], (case, result)
print('PASS: 4096 native explosion variant selections across nine classes and 1..127 variants; CRT multiply/divide mapping')
