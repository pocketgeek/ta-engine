#!/usr/bin/env python3
"""Observe explosion glow mesh/state through the native Glide backend.

Renderer acquisition/capability and exported Glide APIs are sinks. Native
vertex construction, state translation and Glide vertex preparation execute.
"""
import struct
import random
from emu import Icd, HEAP
p=Icd();device,vtable,api=HEAP,HEAP+0x10000,HEAP+0x20000
calls=[];blends=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v))
def draw(uc,sp):
    kind,count,address,stride=struct.unpack('<4I',uc.mem_read(sp,16))
    assert stride==32
    calls.append((kind,[struct.unpack('<4fI3f',uc.mem_read(address+i*32,32)) for i in range(count)]))
    return 4,0
def blend(uc,sp):
    blends.append(struct.unpack('<4I',uc.mem_read(sp,16)));return 4,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),0x5ac370:lambda uc,sp:(0,device),
               0x5ac3a0:lambda uc,sp:(0,0),api:lambda uc,sp:(1,1),api+32:draw,api+48:blend,api+64:lambda uc,sp:(5,0)})
p.freeze_hooks();put(device,vtable);put(vtable+0x20,api);put(vtable+0x6c,0x5b7fe0);put(vtable+0x64,0x5b79a0)
put(device+0xf8,api+48);put(device+0xfc,api+64);put(device+0x118,api+64)
put(device+0xc8,api+32);p.uc.mem_write(device+0x94,struct.pack('<2f',1,1))
rng=random.Random(0x4916d0)
for case in range(1024):
    x,y=rng.randrange(-1000,1000),rng.randrange(-1000,1000)
    rx,ry=rng.randrange(1,500),rng.randrange(1,500)
    alpha=case%256
    calls.clear();blends.clear()
    _,error=p.call(0x4916d0,(x,y,rx,ry,alpha),ecx=HEAP+0x30000)
    assert not error,error
    assert len(calls)==1
    kind,vertices=calls[0]
    assert kind==5 and len(vertices)==14
    assert vertices[0]==(float(x),float(y),0.,1.,(alpha<<24)|0xffffff,0.,0.,0.)
    assert vertices[1]==vertices[-1]
    assert blends[-1:]==[(1,5,1,5)],blends
    for vertex in vertices[1:]:
        assert vertex[2:]==(0.,1.,0xffffff,0.,0.,0.)
        ellipse=((vertex[0]-x)/rx)**2+((vertex[1]-y)/ry)**2
        assert abs(ellipse-1)<0.0002,(case,vertex,ellipse)
    for index in range(6):
        a,b=vertices[1+index],vertices[7+index]
        assert abs(a[0]+b[0]-2*x)<0.0002 and abs(a[1]+b[1]-2*y)<0.0002
print('PASS: 1024 native glows through Glide vertex/state preparation preserve fan geometry and use SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending')
