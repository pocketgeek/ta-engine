#!/usr/bin/env python3
"""Execute retail weapon-loader buildup scaling and its real x87 integer cast."""
import math
import random
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_FPCW

p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
entry, number = HEAP, HEAP + 0x1000
# Synthetic input thunk loads a parsed value; multiplication and conversion are
# the original loader instructions. No executable or asset bytes are embedded.
p.uc.mem_write(entry, b'\xdd\x05' + struct.pack('<I', number) + b'\xe9' +
               struct.pack('<i', 0x531030 - (entry + 11)))
p.hooks[0x53103b] = lambda uc, sp: (0, uc.reg_read(UC_X86_REG_EAX))
p.freeze_hooks()
rng = random.Random(0x531030)
values = [0, .01, .025, .05, .1, .25, .5, 1, 1.25, -.25]
values += [rng.uniform(-10000, 10000) for _ in range(4096)]
for seconds in values:
    p.uc.mem_write(number, struct.pack('<d', seconds))
    result, error = p.call(entry)
    assert not error, error
    expected = math.trunc(seconds * 30.0) & 0xffffffff
    assert result == expected, (seconds, result, expected)
print(f'PASS: {len(values)} native buildup conversions truncate seconds * 30 toward zero')
