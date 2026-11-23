#!/usr/bin/env python3
"""Native flame stream fog admission and per-particle viewport clipping.

Runs 52d710 -> real FlameEmitter draw -> real particle draw and rectangle test.
Only renderer-state calls and final raster submit are sinks. Endpoint and
particle viewport tests and current-sight grid reads execute unchanged.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p=Icd()
game,config,weapon,shot,emitter,head,node,animation,fog,device,vtable = [HEAP+i*0x20000 for i in range(11)]
draws=[]

def put(address,value):p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
def draw(uc,sp):
    draws.append(struct.unpack('<6i',uc.mem_read(sp,24)))
    return 6,0

p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),
    0x5ac370:lambda uc,sp:(0,device),0x5ac3a0:lambda uc,sp:(0,0),
    HEAP+0x180000:lambda uc,sp:(1,0),0x4fac00:draw})
p.freeze_hooks()
put(0x62d55c,game);put(0x62d558,config);put(config+8,config+0x100)
p.uc.mem_write(config+0x115,b'\x01');p.uc.mem_write(game+0x306f,b'\x00')
player=game+0x2404
put(player+0x88,fog);put(player+0x8c,16);put(player+0x90,16)
put(device,vtable);put(vtable+0x50,HEAP+0x180000)
put(shot,weapon);put(weapon+0x54,0);put(shot+0xa8,emitter)
put(emitter,0x5f37b4);put(emitter+8,head);put(head,node);put(node,head)
for offset in (0x174a8,0x174dc,0x174e0):put(game+offset,animation)
p.uc.mem_write(animation,struct.pack('<H',1));put(animation+0x28,123)
rng=random.Random(0x52d710)
rows, results = [], []
for case in range(4096):
    mask=bytes(rng.choice((0,0,1,2,255)) for _ in range(256))
    p.uc.mem_write(fog,mask)
    endpoints=[]
    for offset in (0x10,0x28):
        point=tuple(rng.randrange(-64,576)*65536+rng.randrange(65536) for _ in range(3))
        p.uc.mem_write(shot+offset,struct.pack('<3i',*point));endpoints.append(point)
    position=tuple(rng.randrange(-64,576)*65536+rng.randrange(65536) for _ in range(3))
    p.uc.mem_write(node+8,struct.pack('<6i3I',*position,0,0,0,1,1,case%3))
    camera=(rng.randrange(-32,33),rng.randrange(-32,33))
    put(game+0x14ed0,camera[0]);put(game+0x14ed4,camera[1])
    rect=(0,0,320,240)
    p.uc.mem_write(game+0x19e44,struct.pack('<4i',*rect))
    put(game+0x19e54,320);put(game+0x19e58,240)
    def visible(point):
        x,y,z=(value>>16 for value in point)
        x,z=x>>5,(z-(y>>1))>>5
        return 0<=x<16 and 0<=z<16 and mask[z*16+x]!=0
    x,y,z=(value>>16 for value in position)
    sx,sy=x-camera[0],z-(y>>1)-camera[1]
    def in_view(point):
        x=point[0]>>16
        projected=(point[2]-(point[1]>>1))&0xffffffff
        z=struct.unpack('<h',struct.pack('<H',projected>>16))[0]
        return camera[0]<=x<=camera[0]+320 and camera[1]<=z<=camera[1]+240
    expected=any(map(in_view,endpoints)) and any(map(visible,endpoints)) and 0<=sx<=320 and 0<=sy<=240
    draws.clear()
    _,error=p.call(0x52d710,(shot,),ecx=weapon)
    assert not error,(case,error)
    assert draws==([(123,sx,sy,1,0,0)] if expected else []),(case,draws,expected)
    rows.append(' '.join(map(str, (*endpoints[0],*endpoints[1],*position,*camera,
                                  int(visible(endpoints[0])),int(visible(endpoints[1]))))))
    results.append(int(bool(draws)))
if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--flame-admission'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    assert list(map(int,result.stdout.split()))==results
    print('PASS: C++ stream admission and particle clipping match the complete native draw path')
print('PASS: 4096 native flame draws gate the stream by either projected endpoint, then clip each particle to the viewport without a particle fog test')
