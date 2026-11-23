#!/usr/bin/env python3
"""Run native legacy emitter initialization; sink only its initial spawn call."""
import random
import struct
from emu import Icd, HEAP
p=Icd()
obj,first,second,game=[HEAP+i*0x10000 for i in range(4)]
def put(a,*values):p.uc.mem_write(a,struct.pack('<'+'I'*len(values),*(v&0xffffffff for v in values)))
def read(a,n=1):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
spawns=[]
p.hooks[0x504520]=lambda uc,sp:(spawns.append(1) or (0,0))
p.freeze_hooks();put(0x62d55c,game)
rng=random.Random(0x504420)
def signed(v):return v if v<0x80000000 else v-0x100000000
for case in range(1024):
 duration=6+case%2;tick=rng.randrange(100000)
 a=[rng.randrange(-1000000,1000000) for _ in range(3)]
 b=[rng.randrange(-1000000,1000000) for _ in range(3)]
 put(first,*a);put(second,*b);put(game+0x19f44,tick)
 p.uc.mem_write(obj,bytes(68));put(obj,0x5f2e80);spawns.clear()
 _,error=p.call(0x504420,(first,second,1,duration),ecx=obj)
 assert not error,error
 assert read(obj+4)==(tick+duration,) and read(obj+0x1c)==(1,)
 assert tuple(map(signed,read(obj+0x20,3)))==tuple(a)
 assert tuple(map(signed,read(obj+0x2c,3)))==tuple(b)
 step=tuple(((b[i]-a[i])*(65536//duration))>>16 for i in range(3))
 assert tuple(map(signed,read(obj+0x38,3)))==step,(case,read(obj+0x38,3),step)
 assert spawns==[1]
print('PASS: 1024 native legacy emitter initializations preserve endpoints, set six/seven-tick deadlines and fixed-point endpoint steps, then spawn immediately')
