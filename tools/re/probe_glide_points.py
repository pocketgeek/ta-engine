#!/usr/bin/env python3
"""Execute retail Glide point preparation up to the exported draw-array API.

State is already applied; this observes coordinates, colors and batching,
not the driver's final pixel coverage or blending.
"""
import random
import struct
from emu import Icd, HEAP
p = Icd()
device, vtable, vertices, sink = [HEAP + i * 0x10000 for i in range(4)]
calls = []
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v))
def draw(uc, sp):
    kind, count, address, stride = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert kind == 0 and stride == 32
    calls.append([struct.unpack('<4fI3f', uc.mem_read(address + i * stride, stride))
                  for i in range(count)])
    return 4, 0
p.hooks[sink] = draw
p.freeze_hooks()
put(device, vtable); put(vtable + 0x64, 0x5b79a0)
put(device + 0xc8, sink); put(device + 0x9c, 1)
p.uc.mem_write(device + 0x94, struct.pack('<2f', 1, 1))
rng = random.Random(0x5b79a0)
points = 0
for count in (1, 2, 63, 64, 65, 100, 128, 129):
    for case in range(32):
        source = [(float(rng.randrange(-1000, 1000)), float(rng.randrange(-1000, 1000)),
                   0., 1., rng.getrandbits(32), 0., 0., 0.) for _ in range(count)]
        p.uc.mem_write(vertices, b''.join(struct.pack('<4fI3f', *v) for v in source))
        calls.clear()
        _, error = p.call(0x5b79a0, (1, vertices, count), ecx=device)
        assert not error, error
        assert [len(c) for c in calls] == [min(64, count-i) for i in range(0, count, 64)]
        result = [v for batch in calls for v in batch]
        for before, after in zip(source, result):
            assert after[:2] == (before[0] + .5, before[1] + .5)
            assert after[4] == before[4]
        points += count
print(f'PASS: {points} Glide points retain packed color, add half-pixel XY offsets and split draws into at most 64 vertices')
