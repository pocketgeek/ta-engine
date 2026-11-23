#!/usr/bin/env python3
"""Execute retail model-piece flag admission and palette shading offline.

The body/shadow walkers and scanline shading run original code. Only the final
polygon submission is a sink. Synthetic inputs remain in memory. This verifies
shared model rasterization semantics used by Glide, not a whole-game capture.
"""
import math
import subprocess
import sys
import struct
from emu import Icd, HEAP

p = Icd()
def put(a, v):
    p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

surface, instance, obj, vertices, prim, indices, texture, game = (
    HEAP, HEAP+0x1000, HEAP+0x2000, HEAP+0x3000,
    HEAP+0x4000, HEAP+0x5000, HEAP+0x6000, HEAP+0x10000)
put(0x62d55c, game)
put(0x62d558, HEAP+0x9000)
put(HEAP+0x9018, HEAP+0xa000)
put(instance, 1)
put(instance+0x1cc, obj)
put(instance+0x1f0, vertices)
p.uc.mem_write(instance+0xe4, b'\x01')
put(obj+4, 4)
put(obj+8, 1)
put(obj+0xc, -1)
put(obj+0x28, prim)
for i, (x, y, z) in enumerate([(0,0,0),(16,0,0),(16,0,-16),(0,0,-16)]):
    p.uc.mem_write(vertices+i*12, struct.pack('<iii', x*65536, y*65536, z*65536))
p.uc.mem_write(indices, struct.pack('<4H', 0,1,2,3))
put(prim+4, 4)
put(prim+0xc, indices)
put(prim+0x10, texture)
calls = []
def capture(uc, args):
    calls.append(struct.unpack('<9I', uc.mem_read(args, 36)))
    return 9, 1
p.hooks[0x5415a0] = capture
p.hooks[0x537d30] = lambda uc, args: (1,0)
p.freeze_hooks()
unit = HEAP+0x8000
put(instance+0xc, unit)
put(unit+0xb4, HEAP+0xb000)
put(surface, 64 | (64<<16))
for flags in range(16):
    p.uc.mem_write(instance+0x1f6, struct.pack('<H', flags))
    calls.clear()
    _, error = p.call(0x4eda80, [surface,instance])
    assert not error, error
    assert len(calls) == int((flags & 11) == 11), (flags,calls)
    for cache_pass in (0,1):
        for lighting in (0,1):
            calls.clear()
            _, error = p.call(0x4ed2d0, [surface,instance,0,lighting,cache_pass])
            assert not error, error
            drawn = bool(flags & 1) and bool(flags & 2) == bool(cache_pass)
            assert len(calls) == int(drawn), (flags,cache_pass,lighting,calls)
            if drawn:
                assert calls[0][-1] == (20 if lighting and flags & 4 else 15), calls
print('PASS: all 16 piece flag combinations, both body cache passes, shading on/off, shadow admission')

# Final rendered engine vertices share the native axes: the body heading
# already includes the authored/native half-turn. Compare the production helper
# using the normals of these actual vertices, rather than flipping X/Z twice.
light = struct.unpack('<3f', p.uc.mem_read(0x616440,12))
shade_inputs=[]
shade_expected=[]
p.uc.mem_write(instance+0x1f6,b'\x0f\x00')
for ax in range(-8,9):
    for az in range(-8,9):
        points = [(0,0,0),(16,ax,0),(16,ax+az,-16),(0,az,-16)]
        for i, point in enumerate(points):
            p.uc.mem_write(vertices+i*12,struct.pack('<3i',*(v*65536 for v in point)))
        calls.clear()
        _, error = p.call(0x4ed2d0,[surface,instance,0,1,1])
        assert not error, error
        dot = (-light[0]*ax+light[1]*16+light[2]*az)/math.sqrt(ax*ax+256+az*az)
        expected = 5+int(19*max(0,dot))
        assert len(calls)==1 and calls[0][-1]==expected, (ax,az,calls,expected)
        shade_inputs.append(f'{-ax} 16 {az}')
        shade_expected.append(expected)
binary=sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test'
actual=subprocess.run([binary,'--model-shade'],input='\n'.join(shade_inputs)+'\n',
                      text=True,capture_output=True,check=True)
assert list(map(int,actual.stdout.split()))==shade_expected, 'production model shading differs from retail'
print('PASS: 289 sloped native faces match the production lighting helper')

# Original virtual callbacks: visible/cache/shade/render are bits 0/1/2/3.
vm = HEAP+0x20000
put(vm+0xa64, instance)
for bit, address in enumerate((0x50d7f0,0x50d860,0x50d8c0,0x50d910)):
    for before in range(16):
        for enabled in (0,1):
            p.uc.mem_write(instance+0x1f6, struct.pack('<H', before))
            _, error = p.call(address, [0,enabled], ecx=vm)
            assert not error, error
            after = struct.unpack('<H', p.uc.mem_read(instance+0x1f6,2))[0]
            assert after == (before & ~(1<<bit)) | (enabled<<bit), (bit,before,after)
print('PASS: native SHOW/CACHE/SHADE/RENDER callbacks preserve unrelated flag bits')

# +0x28 is the SHADE TABLE allocated at 547320 and loaded by 5477d0.
# 4c2220 supplies palettes/<name>.shd. A deliberately non-linear synthetic
# table establishes exact indexed lookup, ruling out guessed RGB multipliers.
q = Icd()
def putq(a,v):
    q.uc.mem_write(a, struct.pack('<I',v&0xffffffff))
span, tex, pixels, dst = HEAP, HEAP+0x100, HEAP+0x200, HEAP+0x400
palette, table = HEAP+0x800, HEAP+0x1000
lookup = bytes((level*37+index*13)&255 for level in range(32) for index in range(256))
putq(span,0)
putq(span+4,256)
putq(span+0x10,256*65536)
putq(tex+0x10,pixels)
putq(palette+0x28,table)
q.uc.mem_write(pixels,bytes(range(256)))
q.uc.mem_write(table,lookup)
q.hooks[0x53fe40] = lambda uc,args:(0,palette)
q.freeze_hooks()
for enabled in (0,1):
    for level in range(32):
        q.uc.mem_write(dst,bytes([99])*256)
        _, error = q.call(0x541c10,[span,tex,dst,0,0,0,8,enabled,5,level])
        assert not error, error
        expected = bytearray(lookup[level*256:(level+1)*256] if enabled else bytes(range(256)))
        expected[5] = 99
        assert q.uc.mem_read(dst,256) == expected, (enabled,level)
print('PASS: all 32 shade rows use exact indexed lookup, unlit texels stay raw, transparency preserved')
