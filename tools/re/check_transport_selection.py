#!/usr/bin/env python3
"""Observe air/sea pickup candidate filtering and random selection in retail.

Runs the native mission handlers, ground approach/circle construction, and air
pursuit construction/radius/target routines together; allocator, reference bookkeeping, controller installation,
queue sinks, eligibility and RNG are controlled. Does not emulate the complete
mission dispatcher, terrain navigation or approach movement.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP
p=Icd()
carrier,target,kind,mission,game,mover,reciprocal,owner,pool,buffer,child=[HEAP+i*0x10000 for i in range(11)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def byte(a,v):p.uc.mem_write(a,bytes([v]))
collected=[];selected=[];bounds=[];index=0;eligible=set();approaches=[]

def string(uc,sp):byte(uc.reg_read(UC_X86_REG_ECX),1);return 1,0

def initvec(uc,sp):
 v=uc.reg_read(UC_X86_REG_ECX);put(v+4,buffer);put(v+8,buffer);put(v+12,buffer+1000);return 2,0

def append(uc,sp):
 v=uc.reg_read(UC_X86_REG_ECX);val=read(read(sp+8));collected.append(val);end=read(v+8);put(end,val);put(v+8,end+4);return 3,0

def rand(uc,sp):bounds.append(read(sp));return 1,index

def construct(uc,sp):
 selected.append(read(sp+4));byte(child+4,1);return 12,child
def observe_pursuit(uc,address,size,data):
    sp=uc.reg_read(UC_X86_REG_ESP)+4
    if address==0x4d4da0:
        point,radius=struct.unpack('<2I',uc.mem_read(sp,8))
        approaches.append(('ground',point,radius))
    elif address==0x4e3f70:
        own_mission,passenger=struct.unpack('<2I',uc.mem_read(sp,8))
        approaches.append(('air',own_mission,passenger))
    else:approaches.append(('radius',read(sp)))

def ref_init(uc,sp):
    address=uc.reg_read(UC_X86_REG_ECX);uc.mem_write(address,bytes(16));return 2,address

def ref_bind(uc,sp):
    address=uc.reg_read(UC_X86_REG_ECX);put(address+4,read(sp));return 1,address

p.uc.hook_add(UC_HOOK_CODE,observe_pursuit,begin=0x4d4da0,end=0x4d4da0)
p.uc.hook_add(UC_HOOK_CODE,observe_pursuit,begin=0x4e3f70,end=0x4e3f70)
p.uc.hook_add(UC_HOOK_CODE,observe_pursuit,begin=0x4e4540,end=0x4e4540)

p.hooks.update({0x4d4bf0:string,0x519f50:lambda uc,sp:(1,int(read(sp) in eligible)),0x445a80:lambda uc,sp:(0,buffer),0x42a660:initvec,0x409510:append,0x535cc0:rand,0x4eb9e0:lambda uc,sp:(0,child),0x4d6c40:construct,0x4d7750:lambda uc,sp:(2,0),0x4eba00:lambda uc,sp:(0,0),
    0x519990:ref_init,0x5199f0:ref_bind,
    0x4d4d40:lambda uc,sp:(1,0)})
p.freeze_hooks();put(0x62d55c,game);put(game+0x19f30,1)
put(carrier+0xb4,kind);put(carrier+8,mover);put(carrier+0xb8,owner);put(owner+0x74,pool);put(owner+0x78,pool+15*0x138)
put(target+0xb4,kind);put(target+8,mover);put(target+0x6c,100<<16);put(kind+0x14a,65536)
put(target+0x130,0x1000000);put(target+0x60,reciprocal);byte(reciprocal+4,1);put(reciprocal+0x16,carrier);put(target+0x68,1000<<16)
put(mission+0xe,carrier);put(mission+0x16,target);byte(mission+5,1);p.uc.mem_write(kind+0x23e,struct.pack('<H',150))
rng=random.Random(0x408b5d)
count=0
for handler in (0x408860,0x41a680):
    for case in range(256):
        expected=[];eligible={target}
        for i in range(16):
            u=pool+i*0x138;r=reciprocal+0x100+i*0x100
            valid=rng.choice((True,True,False)) if i else True
            alive=rng.choice((True,True,False)) if i else True
            dying=rng.choice((False,False,True)) if i else False
            mission_ok=rng.choice((True,True,False)) if i else True
            reciprocal_ok=rng.choice((True,True,False)) if i else True
            dx=rng.randrange(-200*65536,200*65536) if i else 50*65536
            dz=rng.randrange(-200*65536,200*65536) if i else 0
            put(u+0x130,(0x1000000 if alive else 0)|(0x1000 if dying else 0))
            put(u+0x60,r);byte(r+4,1 if mission_ok else 2)
            put(r+0x16,carrier if reciprocal_ok else target)
            put(u+0x68,dx);put(u+0x70,dz)
            if valid:eligible.add(u)
            if valid and alive and not dying and mission_ok and reciprocal_ok and ((dx*dx)>>32)+((dz*dz)>>32)<=150*150:
                expected.append(u)
        index=rng.randrange(len(expected))
        collected.clear();selected.clear();bounds.clear()
        ret,error=p.call(handler,(carrier,mission,0))
        assert not error,error
        assert (ret,collected,selected,bounds)==(0,expected,[expected[index]],[len(expected)]),(hex(handler),case,ret,collected,selected,bounds,expected)
        count+=1
print(f'PASS: {count} native air/sea pickup rosters; eligibility, reciprocal missions, range, unit-array order and random selection')

# No nearby candidates: execute the actual native sleep routine too.
put(owner+0x78,pool-1);put(game+0x19f44,100);eligible={target}
count=0;wait_rows=[];wait_expected=[]
for air,handler in ((False,0x408860),(True,0x41a680)):
    for distance in (51,80,150,255,500):
        p.uc.mem_write(kind+0x23e,struct.pack('<H',distance))
        for index in range(6):
            approaches.clear();bounds.clear();put(mission+6,0);put(mission+0xa,0)
            ret,error=p.call(handler,(carrier,mission,0))
            assert not error,error
            expected=[('air',mission,target),('radius',distance-1)] if air else [('ground',target+0x68,distance-16)]
            assert approaches==expected,(air,distance,approaches)
            if air:
                flags,reach=struct.unpack('<Hh',p.uc.mem_read(child+8,4))
                assert (flags,reach)==(0x11,distance-1),(flags,reach)
                point=buffer+0x2000
                _,error=p.call(0x4e41d0,(point,),ecx=child)
                assert not error,error
                assert bytes(p.uc.mem_read(point,12))==bytes(p.uc.mem_read(target+0x68,12))
                original=bytes(p.uc.mem_read(target+0x68,12))
                for step in range(16):
                    position=((1000+step*7)*65536,(100+step*37)*65536,-step*3*65536)
                    p.uc.mem_write(target+0x68,struct.pack('<3i',*position))
                    _,error=p.call(0x4e41d0,(point,),ecx=child)
                    assert not error,error
                    actual_point=struct.unpack('<3i',p.uc.mem_read(point,12))
                    assert actual_point==(position[0],min(position[1],511*65536),position[2])
                p.uc.mem_write(target+0x68,original)
            else:
                assert read(child+4)==mission
                x,z=struct.unpack('<hh',p.uc.mem_read(child+8,4))
                assert (x,z)==(63,0),(x,z)
                assert read(child+12)==distance-16
                assert read(child+16)==((distance-16)**2+128)//256
            assert ret==2 and read(mission+6)==(0x729 if air else 0x709),(air,ret,hex(read(mission+6)))
            assert read(mission+0xa)==100+(index+6 if air else 15),(air,read(mission+0xa))
            assert bounds==([6] if air else []),(air,bounds)
            wait_rows.append(f'{int(air)} {index}')
            wait_expected.append((read(mission+6),read(mission+0xa),6 if air else 0))
            count+=1
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-approach-wait'],
    input='\n'.join(wait_rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==wait_expected,(actual,wait_expected)
print(f'PASS: {count} native pickup approach controllers, radii, event masks and polling deadlines')
print('PASS: mission-created native air pursuit tracks 480 moving-target positions, including height clamp, without reconstructing its controller')

# Surface circles snapshot the passenger position at each approach poll, unlike
# the persistent air pursuit reference. Exercise odd/even footprint origins.
original=bytes(p.uc.mem_read(target+0x68,12))
ground_cases=0
for foot_x,foot_z in ((1,1),(2,4),(3,5),(16,16)):
    put(carrier+0x78,foot_x|(foot_z<<16))
    for x,z in ((1000*65536,0),(-1,1),(17*65536+1,-17*65536-1),(8*65536,24*65536)):
        p.uc.mem_write(target+0x68,struct.pack('<3i',x,100*65536,z))
        _,error=p.call(0x4d4da0,(target+0x68,134),ecx=mission)
        assert not error,error
        expected=((x-foot_x*8*65536+8*65536)>>20,
                  (z-foot_z*8*65536+8*65536)>>20)
        assert struct.unpack('<hh',p.uc.mem_read(child+8,4))==expected
        snapshot=bytes(p.uc.mem_read(child+8,12))
        put(target+0x68,x+32*65536)
        assert bytes(p.uc.mem_read(child+8,12))==snapshot
        _,error=p.call(0x4d4da0,(target+0x68,134),ecx=mission)
        assert not error,error
        assert struct.unpack('<hh',p.uc.mem_read(child+8,4))==(expected[0]+2,expected[1])
        ground_cases+=1
put(carrier+0x78,0);p.uc.mem_write(target+0x68,original)
print(f'PASS: {ground_cases} native ground approach snapshots preserve footprint rounding and only retarget when the mission requests another circle')

# Out-of-range failure handling precedes candidate selection. A stopped
# passenger aborts a surface pickup after controller failure or without a mover.
rows=[];expected=[];index=0
for air,handler in ((False,0x408860),(True,0x41a680)):
    for has_mover in (False,True):
        put(carrier+8,mover if has_mover else 0)
        for events in (0,0x100,0x200,0x400,0x700):
            for speed in (-65536,-1,0,1,65536):
                put(mover+0x20,speed);put(mission+6,0);put(mission+0xa,0)
                ret,error=p.call(handler,(carrier,mission,events))
                assert not error,error
                abort=not air and (not has_mover or bool(events&0x200)) and speed==0
                assert ret==(8 if abort else 2),(air,has_mover,events,speed,ret)
                rows.append(f'{int(air)} {int(has_mover)} {events} {speed}')
                expected.append(int(ret==8))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-approach-abort'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[int(line) for line in result.stdout.splitlines()]
assert actual==expected,(actual,expected)
print(f'PASS: {len(rows)} native pickup approach failure/mover/speed combinations')
