#!/usr/bin/env python3
"""Execute native transient-effect visibility against the real player bitmask.

No substitutions: 4223f0 reads a synthetic visibility grid in native memory.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd()
p.freeze_hooks()
game,player_data,point,grid=[HEAP+i*0x20000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(0x62d55c,game)
width,height=64,48
put(player_data+0x8c,width);put(player_data+0x90,height)
put(game+0x19ef4,grid)
rng=random.Random(0x4223f0)
mask=[rng.getrandbits(16) for _ in range(width*height)]
p.uc.mem_write(grid,struct.pack('<'+'H'*len(mask),*mask))
for case in range(8192):
    player=rng.randrange(16)
    p.uc.mem_write(game+0x306f,bytes([player]))
    coords=[rng.randrange(-128,2304)*65536+rng.randrange(65536),
            rng.randrange(-512,512)*65536+rng.randrange(65536),
            rng.randrange(-256,2048)*65536+rng.randrange(65536)]
    p.uc.mem_write(point,struct.pack('<3i',*coords))
    x,y,z=[v>>16 for v in coords]
    cx,cz=x>>5,(z-(y>>1))>>5
    expected=0<=cx<width and 0<=cz<height and bool(mask[cz*width+cx]&(1<<player))
    result,error=p.call(0x4223f0,(player_data,point))
    assert not error,error
    assert result==int(expected),(case,coords,player,result,expected)
print('PASS: 8192 native effect-visibility queries use projected 32-pixel cells and the selected player bit, with signed bounds checks')
