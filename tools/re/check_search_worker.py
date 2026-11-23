#!/usr/bin/env python3
"""Compare actual scheduler + initialization + trace/cost/retries + delivery.

One controlled allocated entity slot, point goal, and synthetic grade grid.
Only world grid preparation, pending-controller lookup and delivery sinks are
substituted. No path-search phase is substituted. Forced node-cap fixtures
exercise all retries and final failure independent of map connectivity.
"""
import argparse
import random
import struct
import subprocess
from emuphase import Phase, OBJ, GS
from check_cost_search import digest
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP


def case(index,budget,exhaust):
    width,height=40,32; start,goal=(8,8),(30,23)
    if index==0: goal=(30,8)
    if index==1: goal=start
    if index==2: start=(-1,8)
    grades=[6]*(width*height)
    if index in (3,4):
        for z in range(height-(4 if index==3 else 0)): grades[z*width+20]=0
    if index>=5:
        rng=random.Random(0x416430+index)
        grades=[rng.choice((0,4,5,6,6,6,7)) for _ in grades]
    if start[0]>=0: grades[start[1]*width+start[0]]=6
    p=Phase(width,height); unit=p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit,start,goal)
    def write(address,value): p.uc.mem_write(address,struct.pack('<I',value & 0xffffffff))
    config=GS+0x600000
    write(config+8,config+0x100); write(config+0x10c,4)
    p.uc.mem_write(GS+0x3068,b'\x01\x00')
    player=GS+0x2404
    write(player,1); p.uc.mem_write(player+0xea,b'\x01\x00')
    first=unit-0x138
    write(player+0x74,first); write(player+0x78,first+3*0x138)
    write(OBJ+0x115,first); write(OBJ+0x58,0); write(OBJ+0x225,budget)
    pending=[True]; events=[]; queries=[]; routes=[]
    def lookup(uc,args): return 0,p.NAV if pending[0] else 0
    def prepare(uc,args):
        events.extend((2,struct.unpack('<I',uc.mem_read(args+4,4))[0])); return 2,0
    def notify(uc,args):
        events.extend((1,struct.unpack('<I',uc.mem_read(args,4))[0])); return 1,0
    def receive(uc,args):
        address,count=struct.unpack('<II',uc.mem_read(args,8))
        events.extend((3,count))
        routes.append(struct.unpack('<'+'h'*(count*2),uc.mem_read(address,count*4)) if count else ())
        return 2,0
    def finish(uc,args): events.append(4); pending[0]=False; return 1,0
    def grade(uc,args):
        x,z,d=struct.unpack('<iii',uc.mem_read(args,12)); queries.extend((x,z,d))
        return 3,grades[z*width+x] if 0<=x<width and 0<=z<height else 0
    vt=struct.unpack('<I',p.uc.mem_read(p.NAV,4))[0]
    p.icd.hooks[struct.unpack('<I',p.uc.mem_read(vt+0x18,4))[0]]=lookup
    for address,hook in ((0x4e1ee0,prepare),(0x4e2470,notify),(0x4e4ea0,receive),
                          (0x4e2060,finish),(0x4139d0,grade)):
        p.icd.hooks[address]=hook
    if exhaust:
        p.uc.hook_add(UC_HOOK_CODE,lambda uc,a,s,d:write(OBJ+0xec,0),begin=0x415ef3,end=0x415ef3)
    expected=[]
    for tick in range(1,5001):
        write(GS+0x19f44,tick); write(0x634674,int(pending[0]))
        events.clear(); queries.clear()
        _,error=p.icd.call(0x416430,(1,),ecx=OBJ)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        cells=bytes(p.uc.mem_read(p.get(0x1c),width*height*4))
        flags=struct.unpack('<I',p.uc.mem_read(unit+0x134,4))[0]&15
        route=routes[-1] if routes else ()
        values=(tick,p.get(0x165),int(p.get(0x58)!=0),p.get(0x1ad),
            digest(cells[i]|cells[i+1]<<8 for i in range(0,len(cells),4)),digest(queries),digest(events),
            flags,len(route)//2,*route)
        expected.append(' '.join(map(str,values)))
        if not pending[0]: break
    else: raise AssertionError('worker did not finish')
    data=' '.join(map(str,(width,height,*start,*goal,budget,exhaust)))+'\n'
    data+=' '.join(map(str,grades))+'\n'
    return data,expected+['END']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    total=0
    for index in range(10):
        for budget in (7,100,503,12000):
            for exhaust in (0,1):
                data,expected=case(index,budget,exhaust)
                actual=subprocess.run([args.runner,'--worker'],input=data,text=True,capture_output=True,check=True).stdout.splitlines()
                assert actual==expected,(index,budget,exhaust,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected))))
                total+=len(expected)-1
    print(f'PASS: 80 complete search lifecycles, {total} ticks; scheduler, init, retries, queries, cells and delivery match')

if __name__=='__main__': main()
