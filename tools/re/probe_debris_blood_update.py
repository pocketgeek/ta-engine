#!/usr/bin/env python3
"""Compare native blood motion/ground contact with the compiled display helper."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd();particle,game=HEAP,HEAP+0x10000
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
height=0;queries=[];stains=[]
def terrain(uc,sp):
    address=struct.unpack('<I',uc.mem_read(sp,4))[0]
    queries.append(bytes(uc.mem_read(address,12)));return 1,height
def stain(uc,sp):
    count,address,color=struct.unpack('<3I',uc.mem_read(sp,12))
    assert count==1 and color==0xff123456
    stains.append(bytes(uc.mem_read(address,12)));return 3,0
p.hooks.update({0x511170:terrain,0x4f2500:stain});p.freeze_hooks()
put(0x62d55c,game)
rng=random.Random(0x4f1e50);rows=[];expected=[]
for case in range(4096):
    position=[rng.randrange(-20000000,20000000) for _ in range(3)]
    velocity=[rng.randrange(-200000,200000) for _ in range(3)]
    gravity=rng.randrange(20000);sea=case%256;height=rng.randrange(-100,400)
    put(particle+0xc,0xff123456,*position,*velocity)
    put(game+0x19ecc,gravity);p.uc.mem_write(game+0x19ef8,bytes([sea]))
    rows.append(' '.join(map(str,position+velocity+[gravity,sea,height])))
    queries.clear();stains.clear()
    result,error=p.call(0x4f1e50,ecx=particle);assert not error,error
    state=struct.unpack('<6i',p.uc.mem_read(particle+0x10,24))
    assert len(stains)<=1
    if stains:assert stains[0]==bytes(p.uc.mem_read(particle+0x10,12))
    expected.append((result&255,int(bool(stains)),*state,len(queries)))
result=subprocess.run([sys.argv[1],'--blood-step'],input='\n'.join(rows)+'\n',
                      text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected
print('PASS: 4096 compiled/native blood updates match movement, gravity, ground/water removal, stain position and terrain-query count')
