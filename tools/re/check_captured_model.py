#!/usr/bin/env python3
"""Compare every vertex in a captured retail unit's animated model hierarchy.

Uses captured display poses and user-owned authored model data. Native 4eea20
executes the hierarchy; the C++ renderer helpers receive equivalent authored
inputs. This checks geometry, not textures, rasterization or callback timing.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path
from emu import HEAP
from emureload import CapturedProcess
from unicorn.x86_const import UC_X86_REG_EDX
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('capture',type=Path)
ap.add_argument('--unit',type=int,required=True)
ap.add_argument('--model',type=Path,required=True)
ap.add_argument('--binary',default='build-o2/model_transform_test')
a=ap.parse_args()
p=CapturedProcess(json.loads(a.capture.read_text()));p.icd.freeze_hooks()
unit=next(u['address'] for u in p.runtime['units'] if u['id']==a.unit)
root=p.u32(p.u32(unit+0xc0)+0x1c8)
roll,heading,pitch=struct.unpack('<3H',p.uc.mem_read(unit+0x7c,6));body=[pitch,heading,roll]
data=a.model.read_bytes();nodes=[]
def visit(offset,state,ancestors):
    authored=struct.unpack_from('<3i',data,offset+16)
    definition=p.u32(state)
    assert struct.unpack('<3i',p.uc.mem_read(definition+0x10,12))==(-authored[0],authored[1],-authored[2])
    count=struct.unpack_from('<I',data,offset+4)[0]
    assert p.u32(definition+4)==count
    vertex_offset=struct.unpack_from('<I',data,offset+36)[0]
    vertices=[struct.unpack_from('<3i',data,vertex_offset+i*12) for i in range(count)]
    move=struct.unpack('<3i',p.uc.mem_read(state+4,12))
    angles=struct.unpack('<3H',p.uc.mem_read(state+0x10,6))
    values=list(authored)+list(move)+list(angles)+[0,0,0]+body
    chain=ancestors+[values]
    output=p.u32(state+0x24)
    if count:p.uc.mem_write(output,b''.join(struct.pack('<3i',-x,y,-z) for x,y,z in vertices))
    p.uc.mem_write(state+0x28,b'\0\0')
    nodes.append((state,vertices,chain))
    child=struct.unpack_from('<I',data,offset+48)[0];child_state=p.u32(state+0x30)
    while child:
        assert child_state
        visit(child,child_state,chain)
        child=struct.unpack_from('<I',data,child+44)[0];child_state=p.u32(child_state+0x2c)
    assert not child_state
visit(0,root,[])
identity,parent=HEAP+0x300000,HEAP+0x301000
p.uc.mem_write(identity,struct.pack('<12f',1,0,0,0,1,0,0,0,1,0,0,0))
scale=struct.unpack('<f',p.uc.mem_read(0x5f2b28,4))[0]
def bits(v):return struct.unpack('<i',struct.pack('<f',v))[0]
args=[bits(v) for v in (pitch*scale,-heading*scale,roll*scale,0,0,0)]+[identity]
_,error=p.icd.call(0x5ad090,args,ecx=parent);assert not error,error
p.uc.reg_write(UC_X86_REG_EDX,parent)
_,error=p.icd.call(0x4eea20,ecx=root);assert not error,error
rows=[];expected=[]
for state,vertices,chain in nodes:
    output=p.u32(state+0x24)
    for i,vertex in enumerate(vertices):
        values=[list(v) for v in chain];values[-1][9:12]=vertex
        rows.append(str(len(values))+' '+' '.join(str(n) for v in values for n in v))
        expected.append(tuple(v/65536 for v in struct.unpack('<3i',p.uc.mem_read(output+i*12,12))))
assert rows
result=subprocess.run([a.binary,'--chain'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(float,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(expected)
errors=[max(abs(x-y) for x,y in zip(got,want)) for got,want in zip(actual,expected)]
worst=max(range(len(errors)),key=errors.__getitem__)
assert errors[worst]<0.001,(a.unit,worst,actual[worst],expected[worst],errors[worst])
print(f'PASS: unit {a.unit}, {len(nodes)} pieces, {len(rows)} native rendered vertices; maximum error {errors[worst]:.8f}')
