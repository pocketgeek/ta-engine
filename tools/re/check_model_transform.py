#!/usr/bin/env python3
"""Compare renderer piece transforms with native 4eea20's vertex output.

Uses one controlled piece with authored coordinates mirrored by the native
model loader. Root/body and script rotations execute in the retail renderer.
No asset or executable contents are exported.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_EDX,UC_X86_REG_FPCW
p=Icd();p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
identity,parent,node,model,vertices=[HEAP+i*0x1000 for i in range(5)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
put(node,model);put(node+0x24,vertices);put(model+4,2)
p.uc.mem_write(identity,struct.pack('<12f',1,0,0,0,1,0,0,0,1,0,0,0))
scale=struct.unpack('<f',p.uc.mem_read(0x5f2b28,4))[0]
rng=random.Random(0x4eea20);rows=[];expected=[]
for i in range(4096):
    offset=[rng.randrange(-100*65536,100*65536) for _ in range(3)]
    move=[rng.randrange(-20*65536,20*65536) for _ in range(3)]
    angles=[rng.randrange(65536) for _ in range(3)]
    pair=[[rng.randrange(-100*65536,100*65536) for _ in range(3)] for _ in range(2)]
    body=[rng.randrange(65536) for _ in range(3)]
    p.uc.mem_write(model+0x10,struct.pack('<3i',-offset[0],offset[1],-offset[2]))
    p.uc.mem_write(node+4,struct.pack('<3i',*move))
    p.uc.mem_write(node+0x10,struct.pack('<3H',*angles))
    p.uc.mem_write(vertices,b''.join(struct.pack('<3i',-v[0],v[1],-v[2]) for v in pair))
    args=[struct.unpack('<i',struct.pack('<f',v))[0] for v in (f32(body[0]*scale),-f32(body[1]*scale),f32(body[2]*scale),0,0,0)]+[identity]
    _,error=p.call(0x5ad090,args,ecx=parent)
    assert not error,error
    p.uc.reg_write(UC_X86_REG_EDX,parent)
    _,error=p.call(0x4eea20,ecx=node)
    assert not error,error
    for index,vertex in enumerate(pair):
        expected.append(tuple(v/65536 for v in struct.unpack('<3i',p.uc.mem_read(vertices+index*12,12))))
        rows.append(' '.join(map(str,offset+move+angles+vertex+body)))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/model_transform_test','--points'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(float,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(expected)
errors=[max(abs(a-b) for a,b in zip(got,want)) for got,want in zip(actual,expected)]
worst=max(range(len(errors)),key=errors.__getitem__)
assert errors[worst]<0.0005,(worst,rows[worst],actual[worst],expected[worst],errors[worst])
print(f'PASS: 8192 native rendered vertices in 4096 two-vertex pieces; maximum coordinate error {errors[worst]:.8f} world units')
