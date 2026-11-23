#!/usr/bin/env python3
"""Compare full native detached-piece refresh with the port's model transforms.

No native hooks. Synthetic single-piece detached state follows 492910/492590.
Checks two different stored centers per case to test whether they act as pivots.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
unit,view,node,model,source,vertices=[HEAP+i*0x10000 for i in range(6)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
put(unit+0xc0,view);put(view+0x1c8,node);put(node,model)
put(node+0x24,vertices);put(model+4,2);put(model+0x24,source)
rng=random.Random(0x4926e8);rows=[];expected=[]
for case in range(1024):
    move=[rng.randrange(-655360,655360) for _ in range(3)]
    angles=[rng.randrange(65536) for _ in range(3)]
    pair=[[rng.randrange(-6553600,6553600) for _ in range(3)] for _ in range(2)]
    p.uc.mem_write(source,b''.join(struct.pack('<3i',-v[0],v[1],-v[2]) for v in pair))
    put(node+4,*move)
    p.uc.mem_write(node+0x10,struct.pack('<3H',*angles))
    p.uc.mem_write(unit+0x7c,struct.pack('<3H',*angles[::-1]))
    results=[]
    for center in ((0,0,0),tuple(rng.randrange(-10000000,10000000) for _ in range(3))):
        put(view+8,1);put(node+0x18,*center)
        p.uc.mem_write(node+0x28,b'\0\0')
        _,error=p.call(0x4ee620,(unit,))
        assert not error,error
        results.append(bytes(p.uc.mem_read(vertices,24)))
    assert results[0]==results[1],case
    for i,vertex in enumerate(pair):
        expected.append(tuple(v/65536 for v in struct.unpack('<3i',results[0][i*12:i*12+12])))
        rows.append(' '.join(map(str,[0,0,0]+move+angles+vertex+angles)))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/model_transform_test','--points'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(float,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(expected)
errors=[max(abs(a-b) for a,b in zip(got,want)) for got,want in zip(actual,expected)]
worst=max(range(len(errors)),key=errors.__getitem__)
assert errors[worst]<0.0005,(worst,actual[worst],expected[worst],errors[worst])
print(f'PASS: 2048 detached vertices through full native refresh match compiled transforms; max error {errors[worst]:.8f}; stored center does not affect refreshed vertices')
