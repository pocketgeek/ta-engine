#!/usr/bin/env python3
"""Native CRT region/count/kill-selector observations, without a retail GUI."""
import struct
from emu import Icd, HEAP


def main():
    p=Icd(); script=HEAP; regions=HEAP+0x1000; text=HEAP+0x2000
    game=HEAP+0x10000; units=HEAP+0x40000; counters=HEAP+0x50000
    def put(a,f,*v):p.uc.mem_write(a,struct.pack(f,*v))
    def call(a,args):
        result,error=p.call(a,args,ecx=script)
        assert not error,error
        return result
    put(script+0xc,'<II',regions,regions+0x110)
    p.uc.mem_write(regions,b'Hill\0');put(regions+0x100,'<4i',10,20,12,22)
    for name,want in [('Hill',0),('hill',0),('HILL',0),('Hil',0xffffffff),('Hills',0xffffffff),('',0xffffffff),('missing',0xffffffff)]:
        p.uc.mem_write(text,name.encode()+b'\0')
        assert call(0x4cbe50,(text,))==want,name
    put(0x62d55c,'<I',game);put(game+0x2478,'<II',units,units)
    put(units,'<H',17);put(units+0x130,'<I',0x1000000);put(units+0x108,'<f',0)
    for x,z,want in [(10,20,1),(12,22,1),(9,20,0),(13,22,0),(10,19,0),(12,23,0)]:
        put(units+0x74,'<hh',x,z)
        assert call(0x4cbf30,(0,17,0))==want,(x,z,call(0x4cbf30,(0,17,0)),want)
        assert call(0x4cbf30,(0,0,0))==want
        assert call(0x4cbf30,(0,18,0))==0
    put(units+0x74,'<hh',11,21)
    assert call(0x4cbf30,(0,0,-1))==0
    for flags,progress,want in [(0x1000000,0,1),(0x1001000,0,0),(0,0,0),(0x1000000,-1,0),(0x1000000,.5,0)]:
        put(units+0x130,'<I',flags);put(units+0x108,'<f',progress)
        assert call(0x4cbf30,(0,0,0))==want
    for function,start in [(0x4cbdd0,0x4c),(0x4cbe10,0x5c)]:
        put(script+start,'<II',counters,counters+12)
        put(counters,'<HiHi',17,3,18,7)
        for type_id,want in [(17,3),(18,7),(19,0),(0,3)]:
            assert call(function,(0,type_id))==want
    condition=HEAP+0x60000;info=HEAP+0x70000
    put(script,'<I',0)
    for player in range(2):
        slot=game+0x2404+player*0x110
        put(slot,'<I',1);put(slot+0xea,'<BB',1,player);put(slot+0xe8,'<H',1)
        put(slot+0x50,'<I',info+player*0x100)
    counts=[0,0]
    def count_hook(uc,args):
        player=struct.unpack('<I',uc.mem_read(args,4))[0]
        return 3,counts[player]
    def death_hook(uc,args):
        player=struct.unpack('<I',uc.mem_read(args,4))[0]
        return 2,counts[player]
    p.hooks.update({0x4cbf30:count_hook,0x4cbdd0:death_hook,0x4cbe10:death_hook})
    for opcode in (7,8,11,12,13,14):
        put(condition,'<I',opcode)
        for mine,other in ((0,0),(0,1),(1,0),(2,1),(1,1)):
            counts[:]=[mine,other]
            most=opcode in (7,11,13)
            assert bool(call(0x4ca5c0,(condition,))&255)==(mine>other if most else mine<other)
        put(info+0x100+0x9b,'<B',1)
        counts[:]=[0,10]
        assert call(0x4ca5c0,(condition,))&255
        put(info+0x100+0x9b,'<B',0)
    print('PASS: 75 native selector cases (39 region/count/death, 36 strict most/least)')


if __name__=='__main__':main()
