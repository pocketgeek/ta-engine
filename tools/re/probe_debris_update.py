#!/usr/bin/env python3
"""Native debris update oracle: motion, expiry and collision/effect ordering.

Terrain sampling, effect creation and removal are sinks. Optional attached
particle-system updates are excluded; integer movement/rotation execute natively.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
manager,debris,piece,game,weather=[HEAP+i*0x40000 for i in range(5)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a,n):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,4*n))
removed=[];effects=[];samples=[]
terrain=0
def remove(uc,sp):
    removed.append(get(sp,1)[0]);return 1,0
def sample(uc,sp):
    samples.append(get(get(sp,1)[0],3));return 1,terrain
def effect(uc,sp):
    position,kind,light=get(sp,3)
    effects.append((get(position,3),kind,light));return 3,0
p.hooks.update({0x4923d0:remove,0x511260:sample,0x491fd0:effect})
p.freeze_hooks()
put(0x62d55c,game);put(game+0x175dc,weather)
rng=random.Random(0x492420)
rows=[];native=[]
for case in range(4096):
    position=[rng.randrange(-20000000,20000000),rng.randrange(-10,300)*65536+rng.randrange(65536),rng.randrange(-20000000,20000000)]
    velocity=[rng.randrange(-200000,200000) for _ in range(3)]
    rotation=[rng.randrange(65536) for _ in range(3)]
    spin=[rng.randrange(65536) for _ in range(3)]
    life=(0,1,2,900)[case%4]
    sea=case%128;terrain=rng.randrange(-20,300);flags=case%32
    gravity=rng.randrange(10000);wet=case%2;no_splash=case%3
    put(manager+0x2b74,debris);p.uc.mem_write(debris,bytes(0x220))
    put(debris+8,*spin);put(debris+0x14,*velocity)
    put(debris+0x20,life,0,flags,*position);put(debris+0x44,piece)
    p.uc.mem_write(piece+0x10,struct.pack('<3H',*rotation))
    p.uc.mem_write(game+0x19ef8,bytes([sea]));put(game+0x19ecc,gravity)
    put(weather+0xd39,wet,no_splash)
    rows.append(' '.join(map(str,position+velocity+rotation+[spin[1],spin[2],spin[0]]+
                             [life,flags,sea,gravity,wet,no_splash,terrain])))
    removed.clear();effects.clear();samples.clear()
    _,error=p.call(0x492420,ecx=manager)
    assert not error,error
    expected_life=(life-1)&0xffffffff
    assert get(debris+0x20,1)==(expected_life,)
    y=position[1]>>16
    expired=expected_life==0
    water=not expired and y<=sea
    ground=not expired and not water and y+(velocity[1]>>16)<=terrain
    dead=expired or water or ground
    assert removed==([0] if dead else [])
    assert samples==([] if expired or water else [tuple(v&0xffffffff for v in position)])
    expected_effect=[]
    if flags&16 and ((water and not no_splash) or ground):
        kind=(10 if wet else 9) if water else 0
        expected_effect=[(tuple(v&0xffffffff for v in position),kind,0xffffffff if water else 0)]
    assert effects==expected_effect,(case,effects,expected_effect)
    expected_position=position if dead else [position[a]+velocity[a] for a in range(3)]
    assert get(debris+0x2c,3)==tuple(v&0xffffffff for v in expected_position)
    expected_rotation=rotation if dead else [(rotation[a]+spin[(1,2,0)[a]])&65535 for a in range(3)]
    assert struct.unpack('<3H',p.uc.mem_read(piece+0x10,6))==tuple(expected_rotation)
    expected_vy=velocity[1]-(gravity if not dead and flags&8 else 0)
    assert get(debris+0x18,1)==(expected_vy&0xffffffff,)
    def signed(v):return (v+0x80000000)%0x100000000-0x80000000
    impact_kind=effects[0][1] if effects else -1
    impact_light=signed(effects[0][2]) if effects else -1
    native.append([int(not removed),impact_kind,impact_light,get(debris+0x20,1)[0]]+
                  [signed(v) for v in get(debris+0x2c,3)]+[signed(v) for v in get(debris+0x14,3)]+
                  list(struct.unpack('<3H',p.uc.mem_read(piece+0x10,6)))+[len(samples)])
print('PASS: 4096 native debris updates: expiry, water/terrain ordering, impact classes, fixed movement, spin and conditional gravity')

result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test',
                       '--debris-step'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(native),(len(actual),len(native))
for case,(a,b) in enumerate(zip(actual,native)):
    assert a==b,(case,a,b)
print('PASS: 4096 compiled RetailDebrisMotion updates match native state, effects and terrain-query count')
