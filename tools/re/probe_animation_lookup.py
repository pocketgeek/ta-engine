#!/usr/bin/env python3
"""Execute native animation name lookup, including the real string comparison."""
import random
import struct
from emu import Icd, HEAP

p = Icd()
p.freeze_hooks()
container, query, entries = HEAP, HEAP + 0x1000, HEAP + 0x2000
rng = random.Random(0x537550)
for case in range(2048):
    names = [f'effect{rng.randrange(12)}' for _ in range(rng.randrange(17))]
    wanted = rng.choice(names + ['absent', ''])
    names = [''.join(c.upper() if rng.randrange(2) else c for c in n) for n in names]
    wanted = ''.join(c.upper() if rng.randrange(2) else c for c in wanted)
    p.uc.mem_write(container, bytes(128))
    p.uc.mem_write(container + 4, struct.pack('<h', len(names)))
    for i, name in enumerate(names):
        address = entries + i * 128
        p.uc.mem_write(container + 12 + i * 4, struct.pack('<I', address))
        p.uc.mem_write(address + 8, name.encode() + b'\0')
    p.uc.mem_write(query, wanted.encode() + b'\0')
    result, error = p.call(0x537550, (container, query))
    assert not error, (case, error)
    matches = [i for i, name in enumerate(names) if name.lower() == wanted.lower()]
    expected = entries + matches[0] * 128 if matches else 0
    assert result == expected, (case, names, wanted, result, expected)
result, error = p.call(0x537550, (0, query))
assert not error and result == 0
print('PASS: 2048 native animation lookups: case-insensitive first exact match, missing names return null; null container returns null')

# Feature loading uses the same real lookup, then prepares the frame textures.
# GPU preparation is a sink; this checks the surrounding feature-loader path.
p = Icd()
prepared = []
def prepare(uc, sp):
    prepared.append(struct.unpack('<2I', uc.mem_read(sp, 8)))
    return 2, 0
p.hooks[0x4bd840] = prepare
p.freeze_hooks()
p.uc.mem_write(container, bytes(128))
p.uc.mem_write(container + 4, struct.pack('<H', 1))
p.uc.mem_write(container + 12, struct.pack('<I', entries))
p.uc.mem_write(entries + 8, b'feature_test\0')
p.uc.mem_write(query, b'FEATURE_TEST\0')
for loop in range(256):
    p.uc.mem_write(entries + 2, bytes([loop]))
    prepared.clear()
    result, error = p.call(0x493700, (container, 0, query))
    assert not error, error
    assert result == entries and prepared == [(entries, 0)]
    assert p.uc.mem_read(entries + 2, 1) == bytes([loop])
print('PASS: 256 feature-loader paths preserve authored loop metadata around lookup and GPU preparation')
