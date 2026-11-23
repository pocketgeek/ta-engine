#!/usr/bin/env python3
"""Observe native transient-effect draw projection; visibility/raster are sinks."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
head,node,game,query= [HEAP+i*0x20000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
calls=[]
viewport=True
visible=True
def draw(uc,sp):
    calls.append(struct.unpack('<6i',uc.mem_read(sp,24)))
    return 6,0
p.hooks.update({0x48c870:lambda uc,sp:(1,int(viewport)),0x4223f0:lambda uc,sp:(2,int(visible)),
                0x4fac00:draw})
p.freeze_hooks()
put(0x62a38c,head);put(head,node);put(node,head)
put(0x62d55c,game);put(0x62d558,query);put(query+8,query+0x100)
art=HEAP+0xa0000
put(node+0x1c,art)
for frame in range(17):put(art+0x28+frame*8,HEAP+0xb0000+frame*64)
rng=random.Random(0x421d00)
for case in range(4096):
    frame=case%17
    p.uc.mem_write(node+0x14,struct.pack('<H',frame))
    coords=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    camera=[rng.randrange(-32768,32768) for _ in range(2)]
    for i,v in enumerate(coords):put(node+8+i*4,v)
    for i,v in enumerate(camera):put(game+0x14ed0+i*4,v)
    calls.clear()
    _,error=p.call(0x421d00,());assert not error,error
    x,y,z=[v>>16 for v in coords]
    assert calls==[(HEAP+0xb0000+frame*64,x-camera[0],z-(y>>1)-camera[1],0,0,0)],(case,calls)
print('PASS: 4096 native transport-effect projections and actual frame lookups retain whole XYZ, signed half-height and camera offset')
before=bytes(p.uc.mem_read(node,32))
for viewport,visible in ((False,False),(False,True),(True,False),(True,True),
                         (True,False),(True,True)):
    calls.clear()
    _,error=p.call(0x421d00,());assert not error,error
    assert bool(calls)==(viewport and visible)
    assert bytes(p.uc.mem_read(node,32))==before
    assert read(head)==node and read(node)==head
print('PASS: native hidden effect draws preserve the node for later visibility; both draw admission gates apply')

# The alternate fog branch reads its real projected 32-pixel grid directly;
# no visibility helper is substituted on this path.
p.uc.mem_write(query+0x115,b'\x01')
fog=HEAP+0x90000
width,height=64,48
mask=bytes(rng.choice((0,0,0,1,2,255)) for _ in range(width*height))
p.uc.mem_write(fog,mask)
viewport=True
rows, results = [], []
for case in range(4096):
    player=rng.randrange(8)
    p.uc.mem_write(game+0x306f,bytes([player]))
    player_data=game+0x2404+player*0x110
    put(player_data+0x88,fog);put(player_data+0x8c,width);put(player_data+0x90,height)
    coords=[rng.randrange(-128,2304)*65536+rng.randrange(65536),
            rng.randrange(-512,512)*65536+rng.randrange(65536),
            rng.randrange(-256,2048)*65536+rng.randrange(65536)]
    for i,v in enumerate(coords):put(node+8+i*4,v)
    x,y,z=[v>>16 for v in coords]
    cx,cz=x>>5,(z-(y>>1))>>5
    expected=0<=cx<width and 0<=cz<height and mask[cz*width+cx]!=0
    calls.clear()
    _,error=p.call(0x421d00,());assert not error,error
    assert bool(calls)==expected,(case,coords,cx,cz,calls)
    rows.append(' '.join(map(str, coords)))
    results.append(int(bool(calls)))
print('PASS: 4096 native projected-fog lookups use player-specific 32-pixel cells, bounds and nonzero visibility')

if len(sys.argv) > 1:
    prefix = ' '.join(map(str, (width, height, *mask)))
    result = subprocess.run([sys.argv[1], '--effect-visibility'],
        input=prefix+'\n'+'\n'.join(rows)+'\n', text=True, capture_output=True, check=True)
    assert list(map(int, result.stdout.split())) == results
    print('PASS: C++ projected visibility matches all 4096 native draw admissions')
