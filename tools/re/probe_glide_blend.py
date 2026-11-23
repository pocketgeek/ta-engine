#!/usr/bin/env python3
"""Execute retail Glide state translation, sinking only exported Glide APIs.

Complements probe_sprite_blend.py: requested states pass through 5b7fe0 and
5b7e10 to grAlphaBlendFunction. Does not test texture sampling or final pixels.
Enum reference: https://sources.debian.org/src/glide/2002.04.10ds1-25/glide3x/h5/glide3/src/glide.h
"""
import struct
from emu import Icd, HEAP

p = Icd()
device = HEAP
calls = []
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value))
def blend(uc, sp):
    calls.append(struct.unpack('<4I', uc.mem_read(sp, 16)))
    return 4, 0
p.hooks[HEAP + 0x1000] = blend
p.hooks[HEAP + 0x1010] = lambda uc, sp: (5, 0)
p.freeze_hooks()
put(device + 0xf8, HEAP + 0x1000)
put(device + 0xfc, HEAP + 0x1010)
put(device + 0x118, HEAP + 0x1010)
assert bytes(p.uc.mem_read(0x62218c, 24)).split(b'\0')[0] == b'_grAlphaBlendFunction@16'
for textured in (0, 1):
    put(device + 0x90, textured)
    put(device + 0x88, 4)
    for flag in range(256):
        for state, value in ((27, 1), (19, 5), (20, 6 if flag == 255 else 2)):
            _, error = p.call(0x5b7fe0, (state, value), ecx=device)
            assert not error, (state, value, error)
        calls.clear()
        _, error = p.call(0x5b7e10, (), ecx=device)
        assert not error, error
        # Glide: SRC_ALPHA=1, ONE=4, ONE_MINUS_SRC_ALPHA=5.
        dst = 5 if flag == 255 else 4
        assert calls == [(1, dst, 1, dst)], (textured, flag, calls)
print('PASS: 512 Glide state applications confirm format-4 source-alpha blending with destination ONE or ONE_MINUS_SRC_ALPHA')

# Execute Glide's mode setter and textured-quad preparation. Keep real state
# dispatch; texture lookup and final primitive submission are sinks. Capture the
# vertex colors after retail applies its mode-dependent half-alpha override.
p = Icd()
vtable, vertices = HEAP + 0x2000, HEAP + 0x3000
prepared = []
def primitive(uc, sp):
    kind, address, count = struct.unpack('<3I', uc.mem_read(sp, 12))
    assert (kind, count) == (6, 4)
    prepared.append([struct.unpack('<I', uc.mem_read(address + i*32 + 16, 4))[0]
                     for i in range(count)])
    return 3, 0
p.hooks.update({HEAP + 0x1000: blend,
                HEAP + 0x1010: lambda uc, sp: (5, 0),
                HEAP + 0x1020: primitive,
                0x5b9ea0: lambda uc, sp: (3, 0)})
p.freeze_hooks()
put(device, vtable)
put(vtable + 0x6c, 0x5b7fe0)
put(vtable + 0x64, HEAP + 0x1020)
for offset, target in ((0xf8, HEAP + 0x1000), (0xfc, HEAP + 0x1010),
                       (0x118, HEAP + 0x1010)):
    put(device + offset, target)
for mode in (4, 5, 6):
    for alpha in range(256):
        for i in range(4):
            put(vertices + i*32 + 16, (alpha << 24) | 0x123456)
        put(device + 0xac, 0)
        _, error = p.call(0x5b77b0, (mode,), ecx=device)
        assert not error, error
        prepared.clear()
        _, error = p.call(0x5b78f0, (123, vertices), ecx=device)
        assert not error, error
        expected = ((alpha << 24) | 0x123456) if mode == 4 else (
            0x80ffffff if mode == 5 else (alpha << 24) | 0xffffff)
        assert prepared == [[expected]*4], (mode, alpha, prepared)
        calls.clear()
        _, error = p.call(0x5b7e10, (), ecx=device)
        assert not error, error
        assert calls == ([(4, 0, 4, 0)] if mode == 4 else [(1, 5, 1, 5)]), (mode, calls)
print('PASS: 768 Glide textured quads use opaque mode 4, half-alpha mode 5 or caller-alpha mode 6, never additive blending')
