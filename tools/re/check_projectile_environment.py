#!/usr/bin/env python3
"""Native projectile feature/terrain/water collision, including bounce ordering.

Runs 52a4d0 with no unit occupants/designated target. Only map-cell lookup is a
controlled sink; native feature anchor resolution and environmental branches run.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd();game,weapon,shot,cells,features,rules=[HEAP+i*0x10000 for i in range(6)]
cell=cells+14*32
lookup=cell
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
p.hooks[0x50e660]=lambda uc,sp:(1,lookup);p.freeze_hooks()
put(0x62d55c,game);put(shot,weapon);put(game+0x19ec0,3);put(game+0x19edc,features)
put(game+0x19e98,8);put(game+0x175dc,rules)
rng=random.Random(0x52a6f3);rows=[];expected=[]
for i in range(8192):
    minimum=rng.randrange(256);maximum=rng.randrange(minimum,256);sea=rng.randrange(256)
    feature=rng.randrange(256);mode=i%5
    p.uc.mem_write(cell,bytes(14));p.uc.mem_write(cell+5,bytes((maximum,minimum)))
    # Direct, anchored, absent, invalid direct type, and engine sentinel.
    index=(0,0xfffe,0xffff,7,0xfffa)[mode]
    p.uc.mem_write(cell+8,struct.pack('<H',index))
    if mode==1:
        p.uc.mem_write(cell+10,bytes((2,3)))
        p.uc.mem_write(cell-14*(2*8+3)+8,struct.pack('<H',0))
    p.uc.mem_write(features+0x138,bytes((feature,)))
    waterPass=i%7==0;put(rules+0xd3d,int(waterPass));p.uc.mem_write(game+0x19ef8,bytes((sea,)))
    flags=rng.randrange(16)<<11
    whole=rng.choice((minimum,minimum+1,minimum-1,sea,sea-1,minimum+feature,minimum+feature+1,rng.randrange(-400,600)))
    y=whole*65536+rng.randrange(65536);speed=rng.randrange(-0x80000000,0x80000000)
    put(shot+8,y);put(shot+0x20,speed);put(weapon+0xc8,flags)
    result,error=p.call(0x52a4d0,(shot,));assert not error,error
    assert result in (0,2),result
    after=struct.unpack('<i',p.uc.mem_read(shot+0x20,4))[0]
    expected.append((int(result==2),after))
    rows.append(f'{y} {speed} {flags} {minimum} {sea} {feature if mode<2 else -1} {int(waterPass)}')
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--projectile-environment'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
print('PASS: 8192 native projectile environment cases match feature tops, multi-cell anchors, terrain minimum, water boundaries, bypass flags and signed bounce velocity')
