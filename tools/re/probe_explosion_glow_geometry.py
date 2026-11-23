#!/usr/bin/env python3
"""Observe the hardware explosion glow mesh before backend submission.

Renderer acquisition, capability/state calls and final geometry submission are
sinks. Native vertex construction executes; final Glide blending is not covered.
"""
import struct
import random
import subprocess
import sys
from emu import Icd, HEAP
p=Icd();device,vtable,api=HEAP,HEAP+0x10000,HEAP+0x20000
calls=[];states=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v))
def draw(uc,sp):
    kind,address,count=struct.unpack('<3I',uc.mem_read(sp,12))
    calls.append((kind,[struct.unpack('<4fI3f',uc.mem_read(address+i*32,32)) for i in range(count)]))
    return 3,0
def state(uc,sp):
    states.append(struct.unpack('<2I',uc.mem_read(sp,8)));return 2,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),0x5ac370:lambda uc,sp:(0,device),
               0x5ac3a0:lambda uc,sp:(0,0),api:lambda uc,sp:(1,1),api+16:state,api+32:draw})
p.freeze_hooks();put(device,vtable);put(vtable+0x20,api);put(vtable+0x6c,api+16);put(vtable+0x64,api+32)
rng=random.Random(0x4916d0)
rows=[];expected=[]
for case in range(1024):
    x,y=rng.randrange(-1000,1000),rng.randrange(-1000,1000)
    rx,ry=rng.randrange(1,500),rng.randrange(1,500)
    alpha=case%256
    calls.clear();states.clear()
    _,error=p.call(0x4916d0,(x,y,rx,ry,alpha),ecx=HEAP+0x30000)
    assert not error,error
    assert len(calls)==1
    kind,vertices=calls[0]
    rows.append(f'{x} {y} {rx} {ry} {alpha}')
    expected.append([(v[0],v[1],v[4]) for v in vertices])
    assert kind==6 and len(vertices)==14
    assert vertices[0]==(float(x),float(y),0.,1.,(alpha<<24)|0xffffff,0.,0.,0.)
    assert vertices[1]==vertices[-1]
    assert states==[(1,0),(27,1),(19,5),(20,6)]
    for vertex in vertices[1:]:
        assert vertex[2:]==(0.,1.,0xffffff,0.,0.,0.)
        ellipse=((vertex[0]-x)/rx)**2+((vertex[1]-y)/ry)**2
        assert abs(ellipse-1)<0.0002,(case,vertex,ellipse)
    for index in range(6):
        a,b=vertices[1+index],vertices[7+index]
        assert abs(a[0]+b[0]-2*x)<0.0002 and abs(a[1]+b[1]-2*y)<0.0002
print('PASS: 1024 native hardware glow meshes: closed 12-triangle elliptical fan, center alpha, transparent white perimeter and requested blend states')

if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--glow-mesh'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    lines=result.stdout.splitlines();assert len(lines)==len(expected)
    worst=0.
    for case,(line,wanted) in enumerate(zip(lines,expected)):
        values=line.split();assert len(values)==42
        for i,(x,y,color) in enumerate(wanted):
            assert int(values[i*3+2])==color
            error=max(abs(float(values[i*3])-x),abs(float(values[i*3+1])-y))
            worst=max(worst,error)
            assert error<0.0003,(case,i,error)
    print(f'PASS: 14336 compiled glow vertices match native geometry/colors; maximum coordinate error {worst:.8f}')
