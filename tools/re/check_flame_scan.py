#!/usr/bin/env python3
"""Compare native fire initialization ray stepping and retained flight duration.

52d360 runs its complete initialization branch; only collision classification is
controlled. This verifies stepping, clipping handoff and duration, not geometric
collision detection or damage handling inside 52a4d0.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd();game,weapon,projectile,owner=[HEAP+i*0x10000 for i in range(4)]
calls=0;hashvalue=0;hit=0;clip=0
mask=(1<<64)-1
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def collision(uc,sp):
    global calls,hashvalue
    assert struct.unpack('<I',uc.mem_read(sp,4))[0]==projectile
    point=struct.unpack('<3I',uc.mem_read(projectile+4,12));calls+=1
    for v in point:hashvalue=((hashvalue^v)*1099511628211)&mask
    if calls!=hit:return 1,0
    if clip:uc.mem_write(projectile+4,struct.pack('<3I',*((v+i+1)&0xffffffff for i,v in enumerate(point))))
    return 1,2
p.hooks[0x52a4d0]=collision;p.freeze_hooks()
put(0x62d55c,game);put(game+0x19f44,100);put(projectile+0x60,100)
put(projectile,weapon);put(projectile+0x7c,owner);put(owner+0x130,0x1000000)
rng=random.Random(0x52d3fa);rows=[];expected=[]
for i in range(4096):
    origin=tuple(rng.randrange(-0x80000000,0x80000000) for _ in range(3))
    step=tuple(rng.randrange(-32*65536,32*65536) for _ in range(3))
    substeps=rng.randrange(1,9);speed=rng.randrange(8,33)*65536;range_=rng.randrange(1001)
    ticks=(range_<<16)//(speed*substeps)
    hit=rng.choice((0,1,max(1,ticks*substeps),rng.randrange(1,max(2,ticks*substeps+2))))
    clip=i%2
    put(weapon+0x90,range_);put(weapon+0xcc,speed);put(weapon+0xd0,substeps)
    p.uc.mem_write(projectile+4,struct.pack('<3i',*origin));p.uc.mem_write(projectile+0x1c,struct.pack('<3i',*step))
    p.uc.mem_write(projectile+0x97,b'\x7f')
    calls=0;hashvalue=1469598103934665603
    _,error=p.call(0x52d360,(projectile,),ecx=weapon);assert not error,error
    assert struct.unpack('<3i',p.uc.mem_read(projectile+4,12))==origin
    assert bytes(p.uc.mem_read(projectile+0x97,1))==b'\x00'
    endpoint=struct.unpack('<3i',p.uc.mem_read(projectile+0x98,12))
    lifetime=struct.unpack('<I',p.uc.mem_read(projectile+0xb4,4))[0]
    expected.append((calls,lifetime,*endpoint,hashvalue))
    rows.append(' '.join(map(str,(*origin,*step,range_,speed,substeps,hit,clip))))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--flame-scan'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
print('PASS: 4096 native flame scans match all visited positions, collision clipping, endpoint and flight duration; front resets to muzzle and collision flag clears')
