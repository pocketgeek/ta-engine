#!/usr/bin/env python3
"""Execute initialization with controlled controller goals and acceptance.

Weight calculation/type-cost formulas have separate executable checks. This
checks grid preparation order, goal marking/selection, scratch reuse and early
completion, without replacing the initialization routine itself.
"""
import argparse
import random
import struct
import subprocess
from emuphase import Phase, OBJ
from check_cost_search import digest
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    fixtures=[]; expected=[]
    for index in range(96):
        width,height=32,24
        start=(8,8) if index%4 else (-1,8)
        goals=[(20,20),(10,8),(8,10)]
        if index%6==0: goals=[]
        if index%6==1: goals=[(40,8),(-2,8),(32,24)]
        if index%6==2: goals.reverse()
        retry=index%4; accepted=int(index%5==0); distance=123
        weight=(98304,0,20*65536,21*65536)[index%4]
        p=Phase(width,height); unit=p.unit(*start)
        assert p.construct() is None
        p.plant_request(unit,start,(20,20))
        p.uc.mem_write(OBJ+0x38,struct.pack('<ii',6,7))
        p.uc.mem_write(OBJ+0x1ad,struct.pack('<I',retry))
        rng=random.Random(index)
        plane=bytearray()
        for _ in range(width*height): plane.extend((rng.randrange(256),rng.randrange(8),0,0))
        p.uc.mem_write(p.get(0x1c),bytes(plane))
        p.uc.mem_write(p.get(0x2c),b'\xff'*((width*height+255)//256*4))
        events=[]; notifications=[]; finished=[]; routes=[]
        def prepare(uc,args):
            events.extend((1,struct.unpack('<I',uc.mem_read(args+4,4))[0])); return 2,0
        def list_goals(uc,args):
            events.append(2)
            output=struct.unpack('<I',uc.mem_read(args,4))[0]
            address=p._alloc(max(4,len(goals)*4))
            if goals: uc.mem_write(address,b''.join(struct.pack('<hh',*g) for g in goals))
            uc.mem_write(output+4,struct.pack('<III',address,address+len(goals)*4,address+len(goals)*4))
            return 1,output
        def accepts(uc,args): events.append(3); return 2,accepted
        def dist(uc,args): events.append(4); return 2,distance
        def get_weight(uc,args):
            address=struct.unpack('<I',uc.mem_read(args,4))[0]
            uc.mem_write(address,struct.pack('<I',weight)); return 1,address
        def notify(uc,args):
            notifications.append(struct.unpack('<I',uc.mem_read(args,4))[0]); return 1,0
        def finish(uc,args): finished.append(True); return 0,0
        def route(uc,args): routes.append(struct.unpack('<II',uc.mem_read(args,8))); return 2,0
        vt=0x5f28d8
        for address,hook in ((0x4e1ee0,prepare),(0x413e50,dist),(0x4161b0,get_weight),
            (0x4e2470,notify),(0x415f10,finish),(0x4e4ea0,route),
            (struct.unpack('<I',p.uc.mem_read(vt+0x18,4))[0],list_goals),
            (struct.unpack('<I',p.uc.mem_read(vt+0x14,4))[0],accepts)):
            p.icd.hooks[address]=hook
        _,error=p.init()
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        assert routes==([(0,0)] if finished else []),routes
        cells=bytes(p.uc.mem_read(p.get(0x1c),width*height*4))
        values=(int(bool(finished)),500,notifications[-1] if notifications else 0,
                p.get(0x38),p.get(0x3c),p.get(0xb0),p.get(0x54),
                digest(cells[i]|cells[i+1]<<8 for i in range(0,len(cells),4)),digest(events))
        expected.append(' '.join(map(str,values)))
        fixtures.append(' '.join(map(str,(width,height,*start,retry,weight,accepted,distance,6,7,len(goals)))))
        fixtures.extend(f'{x} {z}' for x,z in goals)
        fixtures.extend(f'{plane[i]} {plane[i+1]}' for i in range(0,len(plane),4))
    actual=subprocess.run([args.runner,'--init'],input='\n'.join(fixtures)+'\n',text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected)))
    print(f'PASS: {len(expected)} initializations; grid/controller call order, reused cells, goals and early completion match')

if __name__=='__main__': main()
