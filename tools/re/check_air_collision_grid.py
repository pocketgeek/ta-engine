#!/usr/bin/env python3
"""Compare airborne footprint collision ownership with retail 506c40.

Runs native insertion and overlap-list management. Only the bounded-random
boundary is controlled; the draw count, bounds and selected grid remain checked.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
game,cells,pool=[HEAP+i*0x10000 for i in range(3)]
width=height=16
seed=0;calls=0

def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def draw(uc,sp):
    global seed,calls
    n=struct.unpack('<I',uc.mem_read(sp,4))[0]
    assert 2<=n<=7,n
    seed=(seed*214013+2531011)&0xffffffff;calls+=1
    return 1,((seed>>16)&32767)*n//32768
p.hooks[0x535cc0]=draw;p.freeze_hooks()
put(0x62d55c,game);put(game+0x19e98,width);put(game+0x19e9c,height)
put(game+0x19f04,cells);put(game+0x14e84,pool);put(game+0x14e88,pool+32*0x138)
rng=random.Random(0x506c40);rows=[];expected=[]
for case in range(1024):
    count=rng.randrange(1,25);seed=rng.getrandbits(32);start_seed=seed;calls=0
    p.uc.mem_write(cells,bytes(width*height*14));p.uc.mem_write(pool,bytes(33*0x138))
    bodies=[]
    for i in range(count):
        id=i+1
        # Dense piles exercise the seven-candidate sentinel and sparse cases
        # exercise per-body links shared between different footprint cells.
        x,z=(5,5) if case%4==0 else (rng.randrange(-1,16),rng.randrange(-1,16))
        fx,fz=(3,3) if case%4==0 else (rng.randrange(1,7),rng.randrange(1,7))
        airborne=int(rng.randrange(8)!=0)
        bodies.append((id,x,z,fx,fz,airborne))
        a=pool+id*0x138
        p.uc.mem_write(a+2,struct.pack('<H',id));p.uc.mem_write(a+0x74,struct.pack('<4h',x,z,fx,fz))
        put(a+0x130,0x1000000+(2 if airborne else 1))
        _,error=p.call(0x506c40,(a,));assert not error,(case,id,error)
    native=[]
    b=bytes(p.uc.mem_read(cells,width*height*14))
    for i in range(width*height):
        value=struct.unpack_from('<H',b,i*14+2)[0]
        native.append(-1 if value==65535 else value)
    expected.append((seed,calls,*native))
    rows.append(' '.join(map(str,(width,height,count,start_seed,*[v for body in bodies for v in body]))))
r=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--air-collision-grid'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in r.stdout.splitlines()]
assert actual==expected,next(((i,rows[i],a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),'row count')
print('PASS: 1024 native airborne collision grids match footprint ownership, overlap selection, overflow and random draw counts')
