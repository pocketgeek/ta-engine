#!/usr/bin/env python3
"""Run the owner point-emitter draw through the real Glide point method.

Only renderer acquisition/state application and the final Glide API are
sinks. Native projection, viewport admission and batching execute normally.
"""
import struct
from emu import Icd, HEAP
p=Icd()
emitter,head,nodes,buffer,device,vtable,game=[HEAP+i*0x40000 for i in range(7)]
api=HEAP+0x200000
calls=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def draw(uc,sp):
    kind,count,address,stride=struct.unpack('<4I',uc.mem_read(sp,16))
    assert kind==0 and stride==32
    calls.extend(struct.unpack('<4fI3f',uc.mem_read(address+i*32,32)) for i in range(count))
    return 4,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),0x5ac370:lambda uc,sp:(0,device),
               0x5ac3a0:lambda uc,sp:(0,0),api:draw,api+16:lambda uc,sp:(2,0)})
p.freeze_hooks()
put(0x62d55c,game);put(device,vtable);put(vtable+0x64,0x5b79a0)
put(vtable+0x6c,api+16);put(device+0x9c,1);put(device+0xc8,api)
p.uc.mem_write(device+0x94,struct.pack('<2f',1,1))
p.uc.mem_write(game+0x19e44,struct.pack('<4i',0,0,639,479))
put(game+0x14ed0,10);put(game+0x14ed4,20)
put(emitter+8,head);put(emitter+0x18,buffer)
for routine in (0x502f40,0x4f3330,0x4f3440):
    for count in (1,64,65,100,2048):
        put(head,nodes);put(head+4,nodes+(count-1)*64)
        expected=[]
        for i in range(count):
            node=nodes+i*64;data=node+8
            put(node,node+64 if i+1<count else head)
            x,y,z=(i%140)*5,16,100+(i//140)*3
            put(data+0x10,x*65536+123);put(data+0x14,y*65536+456);put(data+0x18,z*65536+789)
            color=0xff000000 | (i*1777)
            put(data+0xc,color);put(data+8,0)
            if 0<=x-10<=639:expected.append((x-10+.5,z-(y>>1)-20+.5,color))
        calls.clear()
        _,error=p.call(routine,(),ecx=emitter)
        assert not error,error
        actual=[(v[0],v[1],v[4]) for v in calls]
        assert actual==expected,(hex(routine),count,actual,expected)
print('PASS: owner points, moving blood and blood stains reach Glide with correct integer projection, clipping, packed colors and multi-batch ordering')
