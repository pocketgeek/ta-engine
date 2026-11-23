#!/usr/bin/env python3
"""Compare detached child hierarchies through full native model refresh.

Executes native body and piece transforms without substitutions. Generated
fixtures stay in memory; no model or binary data is exported.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p = Icd(); p.freeze_hooks(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
unit, view, nodes, models, vertices, authored = [HEAP+i*0x10000 for i in range(6)]
def put(a,v): p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(unit+0xc0,view);put(view+0x1c8,nodes)
rng = random.Random(0x4eebe9)
rows = []; expected = []
for case in range(1024):
    count = 2 + case % 7
    body = [rng.randrange(65536) for _ in range(3)]
    chain = []
    for i in range(count):
        node,model,vertex_address = nodes+i*128,models+i*128,vertices+i*128
        p.uc.mem_write(node,bytes(128));p.uc.mem_write(model,bytes(128))
        offset = [rng.randrange(-20*65536,20*65536) for _ in range(3)]
        if i==0: offset=[0,0,0] # constructor cancels detached root authored offset
        move = [rng.randrange(-10*65536,10*65536) for _ in range(3)]
        angles = body if i==0 else [rng.randrange(65536) for _ in range(3)]
        vertex = [rng.randrange(-20*65536,20*65536) for _ in range(3)]
        put(node,model);put(node+0x24,vertex_address);put(model+4,1);put(model+0x24,authored+i*128)
        if i+1<count: put(node+0x30,node+128)
        p.uc.mem_write(model+0x10,struct.pack('<3i',-offset[0],offset[1],-offset[2]))
        p.uc.mem_write(node+4,struct.pack('<3i',*move))
        p.uc.mem_write(node+0x10,struct.pack('<3H',*angles))
        p.uc.mem_write(authored+i*128,struct.pack('<3i',-vertex[0],vertex[1],-vertex[2]))
        chain.append(offset+move+angles+vertex+body)
    put(view+8,1)
    p.uc.mem_write(unit+0x7c,struct.pack('<3H',*body[::-1]))
    _,error=p.call(0x4ee620,(unit,));assert not error,error
    for depth in range(1,count+1):
        expected.append(tuple(v/65536 for v in struct.unpack('<3i',p.uc.mem_read(vertices+(depth-1)*128,12))))
        rows.append(str(depth)+' '+' '.join(str(v) for row in chain[:depth] for v in row))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/model_transform_test','--chain'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(float,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(expected)
errors=[max(abs(a-b) for a,b in zip(got,want)) for got,want in zip(actual,expected)]
worst=max(range(len(errors)),key=errors.__getitem__)
assert errors[worst]<0.0005,(worst,rows[worst],actual[worst],expected[worst],errors[worst])
print(f'PASS: {len(rows)} native detached hierarchical vertices at depths 1..8; maximum error {errors[worst]:.8f} world units')
