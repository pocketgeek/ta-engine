#!/usr/bin/env python3
"""Execute native legacy spawning/update with allocation and frame-count sinks.

Uses a synthetic animation with eight frames; does not validate its asset or
rendering. Native particle construction, insertion, stepping and removal run.
"""
import struct
from emu import Icd,HEAP
p=Icd()
obj,first,second,game,particles=[HEAP+i*0x10000 for i in range(5)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a,n=1):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
alloc=[]
def allocate(uc,sp):alloc.append(get(sp)[0]);return 0,particles
p.hooks.update({0x4eb9e0:allocate,0x4eba00:lambda uc,sp:(0,0),0x5367d0:lambda uc,sp:(1,8)})
p.freeze_hooks();put(0x62d55c,game);put(game+0x174b4,99)
checks=0
for duration in (6,7):
 for start in (0,100,100000):
  p.uc.mem_write(obj,bytes(68));put(obj,0x5f2e80);alloc.clear()
  origin=(100*65536,50*65536,-100*65536)
  end=(130*65536,60*65536,-90*65536)
  step=tuple(((b-a)*(65536//duration))>>16 for a,b in zip(origin,end))
  put(first,*origin);put(second,*end);put(game+0x19f44,start)
  _,error=p.call(0x504420,(first,second,1,duration),ecx=obj)
  assert not error,error
  assert alloc==[(duration+1)*60],alloc
  for tick in range(duration+2):
   put(game+0x19f44,start+tick)
   if tick:
    _,error=p.call(0x503380,ecx=obj);assert not error,error
    ready,error=p.call(0x503190,ecx=obj);assert not error,error
    if ready:
     _,error=p.call(0x504520,ecx=obj);assert not error,error
   count=(get(obj+0x14)[0]-get(obj+0x10)[0])//60
   assert count==(tick+1 if tick<=duration else 0),(duration,tick,count)
   for index in range(count):
    row=get(particles+60*index,15);age=tick-index
    expected=(99,*( (origin[i]+step[i]*age)&0xffffffff for i in range(3)),
              *(v&0xffffffff for v in end),*(v&0xffffffff for v in step),
              7,age%7,0,1,start+duration)
    assert row==expected,(duration,tick,index,row,expected)
    checks+=1
  assert alloc==[(duration+1)*60],alloc
print(f'PASS: {checks} native particle snapshots; one spawn per tick through inclusive deadline, fixed-point motion, animated frame cycling, shared expiration and reserved capacity')
