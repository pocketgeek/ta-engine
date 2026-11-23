#!/usr/bin/env python3
"""Compare recursive native rendered vertices with the client hierarchy.

Executes native body and piece transforms without substitutions. Generated
fixtures stay in memory; no model or binary data is exported.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_EDX, UC_X86_REG_FPCW
p = Icd(); p.freeze_hooks(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
identity, parent, nodes, models, vertices = [HEAP+i*0x10000 for i in range(5)]
def put(a,v): p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def f32(v): return struct.unpack('<f',struct.pack('<f',v))[0]
p.uc.mem_write(identity,struct.pack('<12f',1,0,0,0,1,0,0,0,1,0,0,0))
scale = struct.unpack('<f',p.uc.mem_read(0x5f2b28,4))[0]
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
        move = [rng.randrange(-10*65536,10*65536) for _ in range(3)]
        angles = [rng.randrange(65536) for _ in range(3)]
        vertex = [rng.randrange(-20*65536,20*65536) for _ in range(3)]
        put(node,model);put(node+0x24,vertex_address);put(model+4,1)
        if i+1<count: put(node+0x30,node+128)
        p.uc.mem_write(model+0x10,struct.pack('<3i',-offset[0],offset[1],-offset[2]))
        p.uc.mem_write(node+4,struct.pack('<3i',*move))
        p.uc.mem_write(node+0x10,struct.pack('<3H',*angles))
        p.uc.mem_write(vertex_address,struct.pack('<3i',-vertex[0],vertex[1],-vertex[2]))
        chain.append(offset+move+angles+vertex+body)
    args=[struct.unpack('<i',struct.pack('<f',v))[0] for v in
          (f32(body[0]*scale),-f32(body[1]*scale),f32(body[2]*scale),0,0,0)]+[identity]
    _,error=p.call(0x5ad090,args,ecx=parent);assert not error,error
    p.uc.reg_write(UC_X86_REG_EDX,parent)
    _,error=p.call(0x4eea20,ecx=nodes);assert not error,error
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
print(f'PASS: {len(rows)} native hierarchical vertices at depths 1..8; maximum error {errors[worst]:.8f} world units')
