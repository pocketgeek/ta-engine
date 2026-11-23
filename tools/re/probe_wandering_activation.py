#!/usr/bin/env python3
"""Observe native wandering active-loop setup with controlled zero random samples."""
import random
import struct
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
game, weapon, shot, art, zero = [HEAP + i * 0x10000 for i in range(5)]
calls = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def sample(uc, address, size, context):
    sp = uc.reg_read(UC_X86_REG_ESP)
    state, span = struct.unpack('<If', uc.mem_read(sp + 4, 8))
    calls.append((state, span))


# Replace just the random sampler with a synthetic floating-point return sink.
# Animation startup, deadline arithmetic and velocity conversion remain native.
p.uc.mem_write(zero, struct.pack('<d', 0.0))
p.uc.mem_write(0x52f780, b'\xdd\x05' + struct.pack('<I', zero) + b'\xc2\x08\x00')
p.uc.hook_add(UC_HOOK_CODE, sample, begin=0x52f780, end=0x52f780)
p.freeze_hooks()
put(0x62d55c, game);put(weapon + 8, art)
p.uc.mem_write(art, struct.pack('<HB', 2, 1))
p.uc.mem_write(art + 0x2c, struct.pack('<H', 3))
rng = random.Random(0x52f6c0)
for case in range(4096):
    tick, duration, interval = rng.getrandbits(32), rng.randrange(10000), rng.randrange(1000)
    bases = tuple(rng.getrandbits(32) for _ in range(3))
    amplitudes = [struct.unpack('<f', struct.pack('<f', rng.random() * 20))[0] for _ in range(2)]
    p.uc.mem_write(shot, bytes(0x100))
    put(game + 0x19f44, tick);put(weapon + 0x10, duration);put(weapon + 0x14, interval)
    for index, base in enumerate(bases): put(shot + 0xc4 + index * 4, base)
    p.uc.mem_write(shot + 0xbc, struct.pack('<2f', *amplitudes))
    calls.clear()
    _, error = p.call(0x52f6c0, (shot,), ecx=weapon)
    assert not error, (case, error)
    assert (read(shot + 0x70), read(shot + 0x64)) == ((tick + duration) & 0xffffffff, (tick + interval) & 0xffffffff)
    assert read(shot + 0x44) == art
    assert struct.unpack('<HHB', p.uc.mem_read(shot + 0x3c, 5)) == (0, 3, 1)
    expected = ((bases[0] + int(-amplitudes[0] * 65536)) & 0xffffffff,
                bases[1], (bases[2] + int(-amplitudes[1] * 65536)) & 0xffffffff)
    assert struct.unpack('<3I', p.uc.mem_read(shot + 0x1c, 12)) == expected, case
    assert calls == [(shot + 0xb8, amplitudes[0] * 2), (shot + 0xb8, amplitudes[1] * 2)], case
print('PASS: 4096 native wandering activations start loop art and set wrapped active/variation deadlines and XYZ velocity from two controlled samples')
