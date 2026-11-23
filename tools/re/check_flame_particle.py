#!/usr/bin/env python3
"""Compare flame particle construction, expiry, motion and sprite selection.

Only viewport admission and the final draw are substituted. The native particle
constructor, updater and frame/projection calculations execute unchanged.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd()
game,particle,origin,velocity=[HEAP+i*0x10000 for i in range(4)]
animations=[HEAP+(i+4)*0x10000 for i in range(3)]
draws=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def draw(uc,sp):draws.append(struct.unpack('<6I',uc.mem_read(sp,24)));return 6,0
p.hooks.update({0x540150:lambda uc,sp:(3,1),0x4fac00:draw});p.freeze_hooks()
put(0x62d55c,game)
for offset,animation in zip((0x174a8,0x174dc,0x174e0),animations):put(game+offset,animation)
rng=random.Random(0x4f16a0);rows=[];expected=[]
for i in range(4096):
    position=tuple(rng.randrange(-0x80000000,0x80000000) for _ in range(3))
    speed=tuple(rng.randrange(-0x80000000,0x80000000) for _ in range(3))
    life=rng.randrange(1,1001);remaining=rng.randrange(1,life+1);kind=i%4;frames=rng.randrange(1,257)
    p.uc.mem_write(origin,struct.pack('<3i',*position));p.uc.mem_write(velocity,struct.pack('<3i',*speed))
    _,error=p.call(0x4f1790,(origin,velocity,life,kind),ecx=particle);assert not error,error
    assert struct.unpack('<6i3I',p.uc.mem_read(particle,36))==(*position,*speed,life,life,kind)
    put(particle+0x18,remaining)
    for n,a in enumerate(animations):
        p.uc.mem_write(a,struct.pack('<H',frames))
        for f in range(frames):put(a+0x28+f*8,0x100000+n*0x10000+f)
    draws.clear();_,error=p.call(0x4f16e0,(),ecx=particle);assert not error,error
    frame=(life-remaining)*frames//life;sequence=kind if kind in (1,2) else 0
    x,y,z=(v>>16 for v in position)
    assert draws==[(0x100000+sequence*0x10000+frame,x&0xffffffff,(z-(y>>1))&0xffffffff,1,0,0)],draws
    result,error=p.call(0x4f16a0,(),ecx=particle);assert not error,error
    actual=struct.unpack('<3i',p.uc.mem_read(particle,12))
    expected.append((frame,result&255,remaining-1,*actual))
    rows.append(' '.join(map(str,(*position,*speed,life,remaining,frames))))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--flame-particle'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
print('PASS: 4096 native flame particles match construction, signed wrapped motion, expiry-before-motion and lifetime-scaled animation frames; native render projection/sequence selection also checked')
