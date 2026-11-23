#!/usr/bin/env python3
"""Execute native point-particle admission, jitter, velocity and insertion.

Allocation and RNG are the only substitutions. Axis-aligned vectors with
power-of-two lengths give exact expected fixed-point normalized velocities.
"""
import struct
import math
from emu import Icd, HEAP
p = Icd()
emitter, head, origin, target, pool = [HEAP + i * 0x10000 for i in range(5)]
allocated = []
draws = []
random_value = 0
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def read(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
def allocate(uc, sp):
    assert read(sp) == 60
    address = pool + len(allocated) * 64
    allocated.append(address)
    return 1, address

def random_draw(uc, sp):
    draws.append(random_value)
    return 0, random_value
p.hooks.update({0x4f3990: allocate, 0x5d4444: random_draw})
p.freeze_hooks()
base = [100 * 65536 + 123, 50 * 65536 + 321, 200 * 65536 + 456]
p.uc.mem_write(origin, struct.pack('<3i', *base))
cases = 0
for axis in range(3):
    for distance in (-32, -8, 8, 32):
        end = base.copy(); end[axis] += distance * 65536
        p.uc.mem_write(target, struct.pack('<3i', *end))
        for current in (0, 99, 100):
            for requested in (1, 3):
                for period in (8, 16):
                    for random_value in (0, 1, 16384, 32767):
                        allocated.clear(); draws.clear()
                        put(emitter + 8, head); put(emitter + 12, current); put(emitter + 16, 100)
                        put(head, head); put(head + 4, head)
                        _, error = p.call(0x4f19c0, (requested, origin, target, period), ecx=emitter)
                        assert not error, error
                        count = min(requested, 100 - current)
                        assert len(allocated) == count and len(draws) == 3 * count
                        assert read(emitter + 12) == current + count
                        cursor = read(head)
                        jitter = random_value * 7 // 32768 - 3
                        for node in allocated:
                            assert cursor == node
                            data = node + 8
                            position = struct.unpack('<3i', p.uc.mem_read(data + 0x10, 12))
                            motion = struct.unpack('<3i', p.uc.mem_read(data + 0x1c, 12))
                            assert position == tuple(v + jitter * 65536 for v in base)
                            expected = [0, 0, 0]; expected[axis] = 32768 if distance > 0 else -32768
                            assert motion == tuple(expected), (motion, expected)
                            assert read(data + 0x28) == 0
                            assert read(data + 0x2c) == period and read(data + 0x30) == period
                            cursor = read(cursor)
                        assert cursor == head
                        cases += 1
print(f'PASS: {cases} native point-emission cases match capacity 100, three jitter draws, half-unit axis speed, color cadence and list order')

for delta in ((3,4,0),(1,1,1),(-7,13,-2),(31,-8,17)):
    d = [v * 65536 for v in delta]
    p.uc.mem_write(target, struct.pack('<3i', *[a+b for a,b in zip(base,d)]))
    allocated.clear(); draws.clear()
    put(emitter+8,head); put(emitter+12,0); put(emitter+16,100)
    put(head,head); put(head+4,head)
    _, error = p.call(0x4f19c0,(1,origin,target,8),ecx=emitter)
    assert not error,error
    length = math.isqrt(sum(v*v for v in d))
    reciprocal = (1 << 32) // (length * 2)
    expected = tuple((v * reciprocal) >> 16 for v in d)
    motion = struct.unpack('<3i',p.uc.mem_read(allocated[0]+8+0x1c,12))
    assert motion == expected,(delta,motion,expected)
print('PASS: four diagonal emissions match quantized fixed-point normalization')

# Coincident authored vertices still create a stationary particle.
p.uc.mem_write(target,bytes(p.uc.mem_read(origin,12)))
allocated.clear();draws.clear()
put(emitter+8,head);put(emitter+12,0);put(emitter+16,100)
put(head,head);put(head+4,head)
_,error=p.call(0x4f19c0,(1,origin,target,8),ecx=emitter)
assert not error,error
assert len(allocated)==1 and len(draws)==3
assert struct.unpack('<3i',p.uc.mem_read(allocated[0]+8+0x1c,12))==(0,0,0)
print('PASS: coincident endpoints emit one stationary particle with the normal three jitter draws')

# Optional compiled implementation comparison, including arbitrary directions.
import random
import subprocess
import sys
if len(sys.argv)>1:
    rng=random.Random(0x4f1a32)
    inputs=[];expected=[]
    for case in range(1024):
        start=[rng.randrange(-1000*65536,1000*65536) for _ in range(3)]
        end=[v+rng.randrange(-100*65536,100*65536) for v in start] if case else start
        random_value=rng.randrange(32768)
        p.uc.mem_write(origin,struct.pack('<3i',*start))
        p.uc.mem_write(target,struct.pack('<3i',*end))
        allocated.clear();draws.clear()
        put(emitter+8,head);put(emitter+12,0);put(emitter+16,100)
        put(head,head);put(head+4,head)
        _,error=p.call(0x4f19c0,(1,origin,target,8),ecx=emitter)
        assert not error,error
        values=struct.unpack('<6i',p.uc.mem_read(allocated[0]+8+0x10,24))
        inputs.append(' '.join(map(str,start+end+[8,random_value])))
        expected.append(' '.join(map(str,values)))
    result=subprocess.run([sys.argv[1],'--point-emit'],input='\n'.join(inputs)+'\n',
                          text=True,capture_output=True,check=True)
    actual=result.stdout.splitlines()
    assert len(actual)==len(expected)
    for i,(a,b) in enumerate(zip(actual,expected)):
        assert a==b,(i,inputs[i],a,b)
    print('PASS: 1024 compiled C++ emissions match native positions and velocities')
