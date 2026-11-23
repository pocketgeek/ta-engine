#!/usr/bin/env python3
"""Full native debris terrain sample including cell lookup; no routine sinks."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd();p.freeze_hooks();game,cells,position=HEAP,HEAP+0x40000,HEAP+0x80000
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
put(0x62d55c,game);put(game+0x19e98,2,2);put(game+0x19f04,cells)
rng=random.Random(0x511260);rows=[];expected=[]
for case in range(4096):
    heights=[rng.randrange(256) for _ in range(4)]
    for z in range(2):
        for x in range(2):
            corners=[heights[min(z+dz,1)*2+min(x+dx,1)] for dz in (0,1) for dx in (0,1)]
            p.uc.mem_write(cells+(z*2+x)*14+5,bytes([max(corners),min(corners)]))
    x=rng.randrange(-20*65536,40*65536);z=rng.randrange(-20*65536,40*65536)
    put(position,x,0,z)
    value,error=p.call(0x511260,(position,));assert not error,error
    expected.append(value if value<0x80000000 else value-0x100000000)
    rows.append(' '.join(map(str,[x,z,*heights])))
result=subprocess.run([sys.argv[1],'--debris-height'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
assert list(map(int,result.stdout.split()))==expected
print('PASS: 4096 native/compiled debris terrain samples match cell extrema midpoint, signed coordinate truncation and map edges')
