#!/usr/bin/env python3
"""Observe explosion-light envelope and projection before raster submission.

Synthetic envelope records; only viewport admission and the final light draw
are sinks. Debris list and sprite animation are empty. No asset data exported.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
manager,effect,game,settings,options,sight=[HEAP+i*0x40000 for i in range(6)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
calls=[]
def draw(uc,sp):
    calls.append(struct.unpack('<5i',uc.mem_read(sp,20)));return 5,0
p.hooks.update({0x48c870:lambda uc,sp:(1,1),0x4916d0:draw})
p.freeze_hooks()
put(0x62d55c,game);put(0x62d558,settings);put(settings+8,options)
p.uc.mem_write(options+0x15,b'\1')
put(game+0x2404+0x88,sight,64,64);p.uc.mem_write(sight,b'\1'*4096)
put(manager+0x2b5c,effect)
rng=random.Random(0x492139)
rows=[];native=[]
for case in range(4096):
    kind=case%4;duration=1+case%127;elapsed=case%(duration+1)
    start=0xfffffffa if case%4==0 else rng.randrange(10000)
    begin=rng.randrange(2,500);end=rng.randrange(2,500)
    put(0x6112b0+kind*12,duration,begin,end)
    x=rng.randrange(100,1800);y=rng.randrange(100);z=rng.randrange(100,1800)
    camx=rng.randrange(-300,300);camy=rng.randrange(-300,300)
    put(game+0x14ed0,camx,camy);put(game+0x19f44,start+elapsed)
    put(effect+0xc,x*65536+543,y*65536+456,z*65536+234)
    put(effect+0x18,start,kind);p.uc.mem_write(effect+0x20,b'\1')
    calls.clear();_,error=p.call(0x492070,ecx=manager);assert not error,error
    # Native float-to-integer helper truncates toward zero at both stages.
    diameter=int((elapsed/duration)*(end-begin)+begin)
    expected=(x-camx,z-(y>>1)-camy,diameter//2,int(diameter*.75)//2,196)
    assert calls==[expected],(case,calls,expected)
    rows.append(f'{elapsed} {duration} {begin} {end}')
    native.append(calls[0][2:4])
print('PASS: 4096 native explosion-light envelopes: linear size, truncation, height/camera projection, 3:4 vertical scale and alpha 196')

if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--glow-radii'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    assert actual==native
    print('PASS: 4096 compiled glow envelopes match native integer radii exactly')
