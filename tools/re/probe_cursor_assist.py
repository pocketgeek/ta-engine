#!/usr/bin/env python3
"""Run native cursor selection with the real repair/assist eligibility functions.
Synthetic unit/player records; only build-menu membership is a controlled seam.
No retail GUI is launched.
"""
import struct
from emu import HEAP, Icd
p=Icd();uc=p.uc
unit,definition,target,target_def,player,other,game,settings,options,point=[HEAP+i*0x10000 for i in range(1,11)]
def put(a,v): uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def byte(a,v): uc.mem_write(a,bytes([v]))
put(0x62d55c,game);put(0x62d558,settings);put(settings+12,options);byte(options+5,1)
put(unit+0x130,0x01000000);put(unit+0xb4,definition);put(unit+0xb8,player);put(unit+8,1)
put(target+0x130,0x01000001);put(target+0xb4,target_def);put(target+0xb8,player)
byte(player+0xe3,1);byte(player+0xac,1);byte(other+0xe3,1)
# Construction remaining fraction: nonzero is unfinished.
uc.mem_write(target+0x108,struct.pack('<f',0.5))
put(target_def+0x1be,100);uc.mem_write(target+0x10c,struct.pack('<h',100))
allowed=[1]
p.hooks[0x519850]=lambda _uc,_sp:(2,allowed[0])
p.freeze_hooks()
for builder,limited,menu,same,expected in [
    (0,0,1,1,19),(1,1,0,1,19),(1,1,1,1,6),
    (1,0,0,1,6),(1,1,1,0,19)]:
    put(definition+0x260,0x100 if builder else 0)
    put(definition+0x264,0x2000000 if limited else 0)
    put(target+0xb8,player if same else other);allowed[0]=menu
    # Explicit repair mode is mode 8, identified by the native jump table.
    table=struct.unpack('<14I',uc.mem_read(0x4de4f0,56))
    mode=table.index(0x4ddc47)+1
    got,error=p.call(0x4dd780,(mode,unit,target,0))
    assert error is None,(builder,limited,menu,same,error)
    assert got==expected,(builder,limited,menu,same,got,expected)
    print(f'builder={builder} limited={limited} buildable={menu} same-owner={same}: slot {got}')
print('PASS: native repair/assist cursor checks builder capability, ownership and restricted build menu')
