#!/usr/bin/env python3
"""Observe native LineOfSightFire emission timing, moving muzzle and velocity.

Runs 52d360 with collision already resolved. External collision, attachment,
particle allocation/update and CRT results are controlled sinks. This does not
claim collision/damage or final particle-rendering parity.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
game,weapon,projectile,owner,emitter,vtable=[HEAP+i*0x10000 for i in range(6)]
origin=(0,0,0);emissions=[];draws=[];rolls=[];updates=[];destroyed=[];particles_alive=True
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def rand(uc,sp):
    result=rolls[len(draws)];draws.append(result);return 0,result

def muzzle(uc,sp):
    unit,out,slot,piece=struct.unpack('<4I',uc.mem_read(sp,16))
    assert unit==owner and slot==2 and piece==0xffffffff
    uc.mem_write(out,struct.pack('<3i',*origin));return 4,0

def emit(uc,sp):
    count,point,velocity,life=struct.unpack('<4I',uc.mem_read(sp,16))
    emissions.append((count,struct.unpack('<3i',uc.mem_read(point,12)),
                      struct.unpack('<3i',uc.mem_read(velocity,12)),life))
    return 4,0

def update(uc,sp):updates.append(1);return 0,int(particles_alive)
def destroy(uc,sp):
    assert read(sp)==projectile
    destroyed.append(1);return 1,0
p.hooks.update({0x530730:lambda uc,sp:(1,0),0x4dd420:muzzle,0x5d4444:rand,
                0x4f17e0:emit,0x529af0:destroy,HEAP+0x60000:update})
p.freeze_hooks()
put(0x62d55c,game);put(projectile,weapon);put(projectile+0x7c,owner)
put(projectile+0xa8,emitter);put(emitter,vtable);put(vtable+12,HEAP+0x60000)
put(projectile+0xd8,2<<2);p.uc.mem_write(projectile+0x97,b'\x01')
rows=[];expected_velocities=[]
rng=random.Random(0x52d360)
def div(a,b):return abs(a)//b*(-1 if a<0 else 1)
for i in range(4096):
    start=100;duration=rng.randrange(1,61)
    # Start itself has a separate collision pre-scan and is not substituted here.
    now=rng.choice((start-1,start+1,start+duration-1,start+duration,start+duration+1))
    if now==start:now=start-1
    owner_flags=(0x1000000,0,0x1001000,0x1000)[i%4]
    active=owner_flags==0x1000000
    particles_alive=i%8<4
    put(owner+0x130,owner_flags)
    put(game+0x19f44,now);put(projectile+0x60,start);put(projectile+0x70,start+duration)
    life=rng.randrange(1,61);put(projectile+0xb4,life)
    origin=tuple(rng.randrange(-1000*65536,1000*65536) for _ in range(3))
    endpoint=tuple(rng.randrange(-1000*65536,1000*65536) for _ in range(3))
    p.uc.mem_write(projectile+0x98,struct.pack('<3i',*endpoint))
    rolls=[rng.randrange(32768) for _ in range(3)]
    emissions.clear();draws.clear();updates.clear();destroyed.clear()
    _,error=p.call(0x52d360,(projectile,),ecx=weapon);assert not error,error
    expected=active and start<now<start+duration
    assert len(emissions)==int(expected),(i,now,duration,emissions)
    assert len(draws)==3*int(expected)
    if not active:
        assert updates==[1],(owner_flags,now,updates)
        assert len(destroyed)==int(not particles_alive),(owner_flags,now,particles_alive,destroyed)
    if expected:
        base=[div(endpoint[j]-origin[j],life) for j in range(3)]
        velocity=tuple(n+div(div(2*n,20)*rolls[j],32768)-div(n,20) for j,n in enumerate(base))
        assert emissions==[(1,origin,velocity,life)],(i,emissions,velocity)
        rows.append(' '.join(map(str,(*origin,*endpoint,life,*rolls))));expected_velocities.append(velocity)
print('PASS: 4096 native fire emission updates; live/pending-death/dead owner gates, draining after death, one particle per active tick, live muzzle origin, three CRT draws and signed velocity spread')
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--flame-velocity'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
assert [tuple(map(int,line.split())) for line in result.stdout.splitlines()]==expected_velocities
print(f'PASS: shared flame velocity matches {len(rows)} native emitted particles')
