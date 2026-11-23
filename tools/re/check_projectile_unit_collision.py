#!/usr/bin/env python3
"""Compare native 51f340 height and model-selection-quad projectile admission.

Uses an already loaded model, executing native rotation, quantization and strict
polygon tests. No unit-table/map-cell admission or asset loading is substituted
for a claim of full projectile collision parity.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd();unit,kind,model,primitive,vertices,indices,point=[HEAP+i*0x10000 for i in range(7)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(unit+0xb4,kind);put(kind+0x2a0,model);put(model+0xc,0)
put(model+0x28,primitive);put(model+0x24,vertices);put(primitive+0xc,indices)
p.uc.mem_write(indices,struct.pack('<4H',0,1,2,3))
rng=random.Random(0x51f340);rows=[];expected=[];inside=0
for i in range(8192):
    origin=(rng.randrange(-33000,33000)*65536,rng.randrange(-300,301)*65536,rng.randrange(-33000,33000)*65536)
    origin=tuple((v+0x80000000)%0x100000000-0x80000000 for v in origin)
    halfx,halfz=rng.randrange(1,101)*65536,rng.randrange(1,101)*65536
    quad=[(-halfx,-halfz),(halfx,-halfz),(halfx,halfz),(-halfx,halfz)]
    if i%7==0:quad.reverse()
    top=rng.randrange(1,101)*65536;heading=0 if i%4==0 else rng.randrange(65536)
    delta=(rng.choice((0,halfx,halfx-1,-halfx,rng.randrange(-120*65536,120*65536))),
           rng.choice((0,top,top+1,-1,top//2)),
           rng.choice((0,halfz,halfz-1,-halfz,rng.randrange(-120*65536,120*65536))))
    position=tuple((a+b+0x80000000)%0x100000000-0x80000000 for a,b in zip(origin,delta))
    for offset,value in zip((0x68,0x6c,0x70),origin):put(unit+offset,value)
    put(kind+0x14a,top);p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
    p.uc.mem_write(vertices,b''.join(struct.pack('<3i',x,0,z) for x,z in quad))
    p.uc.mem_write(point,struct.pack('<3i',*position))
    result,error=p.call(0x51f340,(unit,point));assert not error,error
    expected.append(result);inside+=result
    rows.append(' '.join(map(str,(*position,*origin,top,heading,*(v for q in quad for v in q)))))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--projectile-in-unit'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[int(v) for v in result.stdout.split()]
assert actual==expected,next(((rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
assert 0<inside<len(rows)
print(f'PASS: {len(rows)} native projectile/unit height and rotated-quad tests ({inside} admitted), including strict edges, winding and signed coordinate wrapping')
