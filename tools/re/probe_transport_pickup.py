#!/usr/bin/env python3
"""Observe air/sea pickup transfer stages in the user-owned retail binary.

Eligibility and external sinks are controlled. This verifies transfer timing,
not complete approach selection or parity of World::tickTransport.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

p=Icd()
carrier,passenger,kind,mission,game,mover,passenger_mission=[HEAP+i*0x10000 for i in range(7)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def byte(a,v):p.uc.mem_write(a,bytes([v]))
controller=HEAP+0x70000
trace=[]
flight_goals=[]
flight_radii=[]
def string(uc,sp):
    byte(uc.reg_read(UC_X86_REG_ECX),1)
    return 1,0

def sleep(uc,sp):trace.append(('sleep',read(sp)));return 1,0

def effect(uc,sp):trace.append(('effect',read(sp+4)));return 2,0

def sound(uc,sp):trace.append(('sound',));return 3,0

def attach(uc,sp):
    args=struct.unpack('<5I',uc.mem_read(sp,20))
    assert args==(passenger,carrier,0xffffffff,0,1),args
    trace.append(('attach',));return 5,0

def flight_goal(uc,sp):
    point=read(sp+4)
    flight_goals.append(tuple(struct.unpack('<3i',uc.mem_read(point,12))))
    return 2,controller

def flight_radius(uc,sp):
    flight_radii.append(read(sp))
    return 1,0

p.hooks.update({0x4d4bf0:string,0x519f50:lambda uc,sp:(1,1),
    0x4d6a10:sleep,0x50a9c0:sound,0x421e10:effect,0x51b4f0:attach,
    0x4eb9e0:lambda uc,sp:(0,controller),0x4e40e0:flight_goal,
    0x4e4540:flight_radius,0x4d4d40:lambda uc,sp:(1,0),
    0x416c50:lambda uc,sp:(4,0)})
p.freeze_hooks()
put(0x62d55c,game);put(game+0x19f30,1)
put(game+0x174c8,101);put(game+0x174cc,102)
put(carrier+0xb4,kind);put(passenger+0xb4,kind)
put(passenger+0x130,0x1000000);put(passenger+8,mover)
put(passenger+0x6c,100<<16);put(kind+0x14a,65536)
put(mission+0x16,passenger);put(passenger+0x60,passenger_mission)
byte(passenger_mission+4,1);put(passenger_mission+0x16,carrier)
rng=random.Random(0x41ad12)
count=0
rows=[];expected_rows=[]
stepout_rows=[];stepout_expected=[]
for air,handler,limit in ((True,0x41a680,4),(False,0x408860,10)):
    for _ in range(2048):
        attempts=rng.randrange(limit+2);ticks=rng.randrange(18)
        speed=rng.choice((-65536,0,1,65536))
        put(mission+0x4e,attempts);put(mission+0x52,ticks)
        byte(mission+5,2);put(mover+0x20,speed);trace.clear()
        result,error=p.call(handler,(carrier,mission,0))
        assert not error,error
        expected=[];expected_attempts=attempts;expected_ticks=ticks
        if attempts>=limit:expected_result=8
        elif speed>0:
            expected_result=2;expected_attempts+=1;expected_ticks=0
            expected=[('sleep',6)]
        elif ticks>=15:expected_result=1
        else:
            expected_result=2;expected_ticks+=1
            if not ticks:expected=[('sound',),('effect',101),('effect',102)]
            expected.append(('sleep',1))
        assert (result,read(mission+0x4e),read(mission+0x52),trace)==(expected_result,expected_attempts,expected_ticks,expected),(air,attempts,ticks,speed,result,trace)
        count+=1
        rows.append(f'{int(air)} {attempts} {ticks} {speed}')
        delay=next((event[1] for event in trace if event[0]=='sleep'),0)
        expected_rows.append((result,expected_attempts,expected_ticks,delay,int(('sound',) in trace)))
    byte(mission+5,3);trace.clear()
    result,error=p.call(handler,(carrier,mission,0))
    assert not error,error
    assert result==5 and trace==[('attach',)],(air,result,trace)
    # These fractional offsets are outside a floating-point circle but inside
    # the native independently-floored squared-distance boundary.
    for radius in range(51,563):
        p.uc.mem_write(kind+0x23e,struct.pack('<H',radius))
        for sign in (-1,1):
            put(carrier+0x68,1000<<16);put(carrier+0x70,1000<<16)
            put(passenger+0x68,(1000+sign*radius)<<16)
            put(passenger+0x70,(1000<<16)+sign*32768)
            heading=rng.randrange(65536)
            p.uc.mem_write(carrier+0x7e,struct.pack('<H',heading))
            byte(mission+5,1);put(passenger+0xd0,0);trace.clear()
            flight_goals.clear();flight_radii.clear()
            result,error=p.call(handler,(carrier,mission,0))
            assert not error,error
            assert result==1 and read(passenger+0xd0)&0x80 and trace==[('sleep',1)],(air,radius,sign,result,trace)
            if air:
                assert len(flight_goals)==1 and flight_radii==[16]
                stepout_rows.append(f'{1000<<16} 0 {1000<<16} {heading} {radius}')
                stepout_expected.append(flight_goals[0])
            else:assert not flight_goals and not flight_radii
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-transfer'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected_rows,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected_rows)) if a!=b),'length mismatch')
stepout=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--unload-stepout'],
    input='\n'.join(stepout_rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split()))[:3] for line in stepout.stdout.splitlines()]
assert actual==stepout_expected,next(((stepout_rows[i],a,b) for i,(a,b) in enumerate(zip(actual,stepout_expected)) if a!=b),'length mismatch')
print(f'PASS: {count} native air/sea pickup transfer states both attachment sinks 2048 fractional pickup boundaries and 1024 air departure goals')

# Drive stages 0..3 with the native sleep deadlines. Ground/air approach is
# already in range; controller/FX sinks remain controlled as above.
expected_timeline=[]
put(carrier+8,mover);put(kind+0x260,0x800)
p.uc.mem_write(kind+0x23e,struct.pack('<H',150))
put(carrier+0x68,400<<16);put(carrier+0x70,400<<16)
put(passenger+0x68,430<<16);put(passenger+0x70,400<<16)
put(mover+0x20,0)
for air,handler in ((False,0x408860),(True,0x41a680)):
    stage=0;deadline=1;attached=False
    put(mission+0x4e,0);put(mission+0x52,0)
    for tick in range(1,19):
        if not attached and tick>=deadline:
            while True:
                byte(mission+5,stage);trace.clear()
                result,error=p.call(handler,(carrier,mission,0))
                assert not error,error
                delay=next((event[1] for event in trace if event[0]=='sleep'),0)
                if result==5:
                    assert trace==[('attach',)],trace
                    attached=True;break
                assert result in (1,2),(air,tick,stage,result,trace)
                if result==1:stage+=1
                if delay:
                    deadline=tick+delay;break
                assert result==1,(air,tick,stage,result,trace)
        expected_timeline.append((int(air),tick,-1 if attached else stage,
            0 if attached else read(mission+0x52),0 if attached else read(mission+0x4e),int(attached)))
timeline=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-timeline'],
    text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in timeline.stdout.splitlines()]
assert actual==expected_timeline,next(((a,b) for a,b in zip(actual,expected_timeline) if a!=b),'length mismatch')
print('PASS: full stationary in-range pickup timeline for air and sea, initialization through attachment')
