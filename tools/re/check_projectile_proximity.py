#!/usr/bin/env python3
"""Verify the optional projectile-interception proximity branch of 52a4d0.

Map lookup is supplied, unit occupants are absent and environment is bypassed.
All fixed-point subtraction, squared-distance truncation and comparison execute
inside the native collision routine.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd();game,weapon,shot,target,cell=[HEAP+i*0x10000 for i in range(5)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def signed(v):return (v+0x80000000)%0x100000000-0x80000000
p.hooks[0x50e660]=lambda uc,sp:(1,cell);p.freeze_hooks()
put(0x62d55c,game);put(shot,weapon);put(shot+0x84,target);put(weapon+0xc8,0x800)
rng=random.Random(0x52a519);rows=[];expected=[]
for i in range(8192):
    origin=tuple(rng.randrange(-0x80000000,0x80000000) for _ in range(3))
    radius=rng.randrange(65536)
    if i%3==0:
        delta=(signed(radius*65536+rng.choice((-1,0,1))),0,0)
    else:delta=tuple(rng.randrange(-0x80000000,0x80000000) for _ in range(3))
    point=tuple(signed(a+b) for a,b in zip(origin,delta))
    p.uc.mem_write(target+4,struct.pack('<3i',*origin));p.uc.mem_write(shot+4,struct.pack('<3i',*point))
    p.uc.mem_write(weapon+0x8a,struct.pack('<H',radius));put(shot+0x80,0xdeadbeef)
    result,error=p.call(0x52a4d0,(shot,));assert not error,error
    assert result in (0,2),result
    assert struct.unpack('<I',p.uc.mem_read(shot+0x80,4))[0]==0
    expected.append(int(result==2));rows.append(' '.join(map(str,(*point,*origin,radius))))
r=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--projectile-proximity'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[int(line) for line in r.stdout.splitlines()]
assert actual==expected,next(((i,rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
print('PASS: 8192 native interception proximity cases match strict radius, per-axis truncation and signed wrapping')
