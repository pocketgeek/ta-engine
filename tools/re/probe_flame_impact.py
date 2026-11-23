#!/usr/bin/env python3
"""Native fire timeline with a controlled obstacle and impact/particle sinks.

52d360 retains its real scan/front/emission sequencing. Collision geometry and
529c10's damage application are substituted; only impact dispatch timing is
established here, not damage magnitudes or target eligibility.
"""
import struct
from emu import Icd,HEAP
p=Icd();game,weapon,shot,owner,emitter,vtable=[HEAP+i*0x10000 for i in range(6)]
now=0;obstacle=0;impacts=[];emissions=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def collision(uc,sp):
    assert read(sp)==shot
    if read(shot+4)<obstacle:return 1,0
    put(shot+4,obstacle);put(shot+0x80,123)
    return 1,2

def impact(uc,sp):
    args=struct.unpack('<5I',uc.mem_read(sp,20))
    assert args==(shot,123,0,1,0),args
    impacts.append(now);return 5,0

def muzzle(uc,sp):
    assert read(sp)==owner
    uc.mem_write(read(sp+4),bytes(12));return 4,0

def emit(uc,sp):emissions.append(now);return 4,0
p.hooks.update({0x52a4d0:collision,0x529c10:impact,0x530730:lambda uc,sp:(1,0),
    0x4dd420:muzzle,0x4f17e0:emit,0x5d4444:lambda uc,sp:(0,16384),
    HEAP+0x70000:lambda uc,sp:(0,1)})
p.freeze_hooks();put(0x62d55c,game);put(shot,weapon);put(shot+0x7c,owner)
put(owner+0x130,0x1000000);put(shot+0xa8,emitter);put(emitter,vtable);put(vtable+12,HEAP+0x70000)
count=0
for substeps in (1,2,3,4):
    for distance in range(1,17):
        speed=16*65536;obstacle=distance*speed-speed//3
        put(weapon+0x90,500);put(weapon+0xcc,speed);put(weapon+0xd0,substeps)
        put(shot+0x60,100);put(shot+0x70,130)
        p.uc.mem_write(shot+4,bytes(12));p.uc.mem_write(shot+0x1c,struct.pack('<3i',speed,0,0))
        p.uc.mem_write(shot+0x97,b'\0');impacts.clear();emissions.clear()
        for now in range(100,132):
            put(game+0x19f44,now)
            _,error=p.call(0x52d360,(shot,),ecx=weapon);assert not error,error
            if now==100:
                assert not impacts and not emissions
                assert read(shot+4)==0 and read(shot+0x98)==obstacle
        arrival=100+(distance+substeps-1)//substeps
        assert impacts==[arrival],(distance,substeps,impacts,arrival)
        assert emissions==list(range(101,130)),emissions
        count+=1
print(f'PASS: {count} native fire timelines pre-scan without impact, advance the front in substeps, dispatch one delayed impact, and continue emitting through emittime')
