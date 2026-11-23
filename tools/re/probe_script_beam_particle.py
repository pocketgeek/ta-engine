#!/usr/bin/env python3
"""Verify native script-beam particle motion, frame cycling and expiry.

Executes both routines without substitutions. Rendering is a separate gate.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
p.freeze_hooks()
rng = random.Random(0x5042a0)
def put(offset, value):
    p.uc.mem_write(HEAP + offset, struct.pack('<I', value & 0xffffffff))
def get(offset):
    return struct.unpack('<I', p.uc.mem_read(HEAP + offset, 4))[0]
def signed(value):
    return (value + 0x80000000) % 0x100000000 - 0x80000000
steps = 0
for case in range(1024):
    position = [rng.getrandbits(32) for _ in range(3)]
    velocity = [rng.getrandbits(32) for _ in range(3)]
    count, period = rng.randrange(1, 30), rng.randrange(1, 12)
    frame, phase = rng.randrange(count), rng.randrange(period)
    deadline = rng.getrandbits(32)
    for i in range(3):
        put(4 + i * 4, position[i])
        put(0x1c + i * 4, velocity[i])
    for offset, value in ((0x28, count), (0x2c, frame), (0x30, phase),
                          (0x34, period), (0x38, deadline)):
        put(offset, value)
    for tick in range(32):
        _, error = p.call(0x5042a0, (), ecx=HEAP)
        assert not error, error
        position = [(a + b) & 0xffffffff for a, b in zip(position, velocity)]
        phase = (phase + 1) % period
        if phase == 0:
            frame = (frame + 1) % count
        assert [get(4 + i * 4) for i in range(3)] == position
        assert (get(0x2c), get(0x30)) == (frame, phase)
        now = (deadline + tick - 16) & 0xffffffff
        expired, error = p.call(0x504400, (signed(now),), ecx=HEAP)
        assert not error, error
        assert expired == int(signed(now) > signed(deadline))
        steps += 1
print(f'PASS: {steps} native script-beam particle updates match wrapped motion, frame cycling and strict signed deadline expiry')
