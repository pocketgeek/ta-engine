#!/usr/bin/env python3
"""Compare real wandering initialization geometry with the shared launch helper.

Only terrain lookup and caster-animation startup are sinks. The real base
initializer runs; the factory's stored aim point is supplied in shot +28.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
game,weapon,config,shot,owner,aim=[HEAP+i*0x10000 for i in range(6)]
height=0;lookups=[]


def put(address,value):p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
def read(address):return struct.unpack('<I',p.uc.mem_read(address,4))[0]
def signed(value):return (value+0x80000000)%0x100000000-0x80000000

def terrain(uc,sp):
    point=read(sp)
    assert point==shot+4
    lookups.append(struct.unpack('<3i',uc.mem_read(point,12)))
    return 1,height

p.hooks[0x511170]=terrain
p.freeze_hooks();put(0x62d55c,game)
rng=random.Random(0x52f970);rows=[];expected=[]
for case in range(8192):
    source=tuple(signed(rng.getrandbits(32)) for _ in range(3))
    target=tuple(signed(rng.getrandbits(32)) for _ in range(3))
    if case%16==0:target=source
    if case%16==1:target=(source[0],target[1],target[2])
    speed=rng.randrange(1,1048577);steps=rng.randrange(1,9)
    variation=rng.randrange(-32,33);height=rng.randrange(256)
    heading,pitch=rng.randrange(65536),rng.randrange(65536)
    p.uc.mem_write(shot,bytes(0x100));put(shot,weapon)
    p.uc.mem_write(shot+0x28,struct.pack('<3i',*target))
    p.uc.mem_write(owner+0x68,struct.pack('<3i',*source))
    p.uc.mem_write(aim+0x16,struct.pack('<HH',heading,pitch))
    put(weapon+0xcc,speed);put(weapon+0xd0,steps);put(weapon+0xc8,0)
    put(config+0x18,variation);put(game+0x19f44,123)
    lookups.clear()
    _,error=p.call(0x52f970,(shot,owner,aim),ecx=config)
    assert not error,(case,error)
    position=list(struct.unpack('<3i',p.uc.mem_read(shot+4,12)))
    assert len(lookups)==1 and lookups[0][0]==position[0] and lookups[0][2]==position[2]
    assert position[1]==height*65536
    assert read(shot+0xb8)==heading|(pitch<<16)
    assert read(shot+0x60)==124 and read(shot+0x74)==123
    position[1]=0 # Shared geometry helper leaves the terrain query to its caller.
    velocity=list(struct.unpack('<3i',p.uc.mem_read(shot+0xc4,12)))
    amplitudes=list(struct.unpack('<2I',p.uc.mem_read(shot+0xbc,8)))
    rows.append(' '.join(map(str,(*source,*target,speed,steps,variation))))
    expected.append(position+velocity+amplitudes)
if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--storm-launch'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    assert len(actual)==len(expected)
    for index,(got,want) in enumerate(zip(actual,expected)):
        assert got==want,(index,rows[index],got,want)
print('PASS: 8192 native wandering launches match whole-coordinate normalization, float boundaries, origin, base velocity and signed amplitudes; terrain/seed/timing wiring verified')
