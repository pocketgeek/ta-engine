#!/usr/bin/env python3
"""Execute native smoke constructor/update/draw; only RNG and final draw sink.

Tests wind/rise, randomized frame countdown, expiry, bank choice and half-alpha
call flag. Emission/list ownership remains a separate integration check.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
particle,origin,game=HEAP,HEAP+0x1000,HEAP+0x10000
random_value=0
draws=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def signed(v):return v if v<0x80000000 else v-0x100000000
def draw(uc,sp):
    draws.append(struct.unpack('<6i',uc.mem_read(sp,24)))
    return 6,0
p.hooks.update({0x5d4444:lambda uc,sp:random_draw(uc,sp),0x4fac00:draw})
p.freeze_hooks()
put(0x62d55c,game)
for i,offset in enumerate((0x174a0,0x174a4,0x174e4)):
    bank=HEAP+0x40000+i*0x1000
    put(game+offset,bank)
    for frame in range(32):put(bank+0x28+8*frame,1000+i*100+frame)
# Inclusive native viewport covers all generated projected positions.
for i,v in enumerate((-100000,-100000,100000,100000)):put(game+0x19e44+i*4,v)
rng=random.Random(0x4f1bd0)
steps=0
rows=[];results=[]
rng_calls=0
def random_draw(uc,sp):
    global rng_calls
    rng_calls+=1
    return 0,random_value

for case in range(512):
    position=[rng.getrandbits(32) for _ in range(3)]
    wind=[rng.randrange(-8192,8193) for _ in range(2)]
    gravity=rng.randrange(1,8193)
    period=rng.randrange(2,17);limit=rng.randrange(2,30)
    flags=(case%2,(case//2)%2)
    for i,v in enumerate(position):put(origin+4*i,v)
    put(game+0x19f60,wind[0]);put(game+0x19f68,wind[1]);put(game+0x19ecc,gravity)
    _,error=p.call(0x4f1cf0,(origin,period,limit,*flags),ecx=particle)
    assert not error,error
    remaining=period;frame=0
    for tick in range(600):
        random_value=rng.randrange(32768)
        rows.append(" ".join(map(str,(*map(signed,position),period,limit,remaining,frame,*wind,gravity,random_value))))
        rng_calls=0
        position=[(position[0]+wind[0]*8)&0xffffffff,
                  (position[1]+gravity*4)&0xffffffff,
                  (position[2]+wind[1]*8)&0xffffffff]
        remaining-=1
        alive=True
        if remaining==0:
            half=period//2
            remaining=half+random_value*half//32768
            frame+=1
            alive=frame!=limit
        value,error=p.call(0x4f1bd0,(),ecx=particle)
        assert not error,error
        assert bool(value&255)==alive,(case,tick,value,alive)
        assert [read(particle+1+4*i) for i in range(3)]==position
        assert (read(particle+0x15),read(particle+0x19))==(remaining,frame)
        results.append((*map(signed,position),remaining,frame,int(alive),rng_calls))
        steps+=1
        if not alive:break
        draws.clear()
        _,error=p.call(0x4f1c60,(),ecx=particle)
        assert not error,error
        bank=2 if flags[1] else int(bool(flags[0]))
        x,y,z=[signed(v)>>16 for v in position]
        assert draws==[(1000+bank*100+frame,x,z-(y>>1),1,0,0)],(case,tick,draws)
    else:raise AssertionError('particle did not expire')
print(f'PASS: {steps} native smoke steps across 512 lifetimes match drift, rise, frame clock, expiry and half-alpha draws')

# Retail calls 540150 before 4fac00. The viewport rectangle includes its four
# edges; a smoke sprite is admitted by its projected center, not its bitmap.
viewport=(10,20,30,40)
edge_cases=((9,20,False),(10,20,True),(30,20,True),(31,20,False),
            (20,19,False),(20,20,True),(20,40,True),(20,41,False))
for sx,sy,admitted in edge_cases:
    for i,value in enumerate(viewport):put(game+0x19e44+4*i,value)
    p.uc.mem_write(origin,struct.pack('<3I',sx<<16,0,sy<<16))
    _,error=p.call(0x4f1cf0,(origin,100,30,0,0),ecx=particle)
    assert not error,error
    draws.clear()
    _,error=p.call(0x4f1c60,(),ecx=particle)
    assert not error,error
    assert bool(draws)==admitted,(sx,sy,draws,admitted)
print('PASS: native smoke draw admission is inclusive at every viewport edge and uses particle center')

if len(sys.argv)>1:
    output=subprocess.run([sys.argv[1],'--smoke-step'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in output.stdout.splitlines()]
    assert actual==results
    print('PASS: compiled smoke particle matches every native update and RNG call count')
    rows=[' '.join(map(str,(x-viewport[0],y-viewport[1],
                            viewport[2]-viewport[0],viewport[3]-viewport[1])))
          for x,y,_ in edge_cases]
    output=subprocess.run([sys.argv[1],'--smoke-viewport'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[line.strip()=='1' for line in output.stdout.splitlines()]
    expected=[admitted for _,_,admitted in edge_cases]
    assert actual==expected,(actual,expected)
    print('PASS: compiled renderer viewport helper matches native inclusive-edge admission')
