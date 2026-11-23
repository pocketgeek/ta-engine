#!/usr/bin/env python3
"""Observe native building footprint occupancy, with synthetic flat terrain.

Runs 507d10 and its building branch 507400 without substituted routines.
This does not verify terrain restrictions or authored yardmap mask decoding.
"""
import struct
from emu import Icd, HEAP
p=Icd()
game,kind,cells,entities,masks=[HEAP+i*0x20000 for i in range(5)]
def put(address,*values):
    p.uc.mem_write(address,struct.pack('<'+'I'*len(values),*values))
put(0x62d55c,game)
put(game+0x19e98,32,32)
put(game+0x19f04,cells)
put(game+0x14e84,entities,entities+3*312)
p.uc.mem_write(game+0x19ef8,b'\x14')
p.uc.mem_write(kind+0x192,struct.pack('<hh',255,-255))
p.uc.mem_write(kind+0x23c,b'\xff')
put(kind+0x12a,masks)
p.uc.mem_write(entities+312+2,struct.pack('<H',1))
put(entities+312+0x130,0x1000000)
p.freeze_hooks()
cases=0
for fx,fz in [(7,12),(12,7),(1,1),(3,5)]:
    p.uc.mem_write(kind+0x126,struct.pack('<hh',fx,fz))
    for mask in [0,2,4,6,14]:
        p.uc.mem_write(masks,bytes([mask])*(fx*fz))
        for x,z in [(7,8),(8,7),(8+fx,8),(8,8+fz),(8,8),(8+fx-1,8+fz-1)]:
            raw=bytearray(32*32*14)
            for i in range(32*32):
                raw[i*14+5:i*14+7]=bytes([100,100])
                raw[i*14+8:i*14+10]=b'\xff\xff'
            struct.pack_into('<H',raw,(z*32+x)*14,1)
            p.uc.mem_write(cells,bytes(raw))
            for moving in [0,1]:
                put(entities+312+8,moving)
                for allow_moving in [0,1]:
                    result,error=p.call(0x507d10,(kind,0,(8<<16)|8,1,allow_moving))
                    assert not error,error
                    inside=8<=x<8+fx and 8<=z<8+fz
                    expected=int(not (inside and mask&6 and not (moving and allow_moving)))
                    assert result==expected,(fx,fz,mask,x,z,moving,allow_moving,result,expected)
                    cases+=1
print(f'PASS: {cases} native building occupancy cases; outside cells do not block, mask bits 2/4 gate inside occupancy')
