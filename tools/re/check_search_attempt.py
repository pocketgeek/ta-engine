#!/usr/bin/env python3
"""Compare trace-to-cost handoff and delivery against executable retail.

World grade queries are controlled; trace, seed, cost slice and reconstruction
instructions execute unchanged. Stops at the scheduler's retry boundary.
"""
import argparse
import random
import struct
import subprocess
from emuphase import Phase, OBJ
from emu import STACK, STACK_SZ
from check_cost_search import digest
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EIP, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_ESI, UC_X86_REG_EBX, UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX)


def case(index, budget, retry):
    width, height = 40, 32
    start, goal = (8, 8), (30, 23)
    if index == 0: goal = (30, 8)
    if index == 1: start = (1, 1)
    grades = [6] * (width*height)
    rng = random.Random(0x415b10+index)
    if index == 2:
        for z in range(height-4): grades[z*width+20] = 0
    if index == 3:
        for z in range(height): grades[z*width+20] = 0
    if index == 4:
        for z in range(7,10):
            for x in range(7,10): grades[z*width+x] = 0
    if index >= 5:
        grades = [rng.choice((0,4,5,6,6,6,7)) for _ in grades]
    grades[start[1]*width+start[0]] = 6
    if index == 12:
        width,height=16,12; start,goal=(1,1),(12,8)
        grades=[6 if (z==1 and 1<=x<=12) or (x==12 and 1<=z<=8) else 0
                for z in range(height) for x in range(width)]
    p = Phase(width,height)
    unit = p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit,start,goal)
    p.uc.mem_write(OBJ+0x1ad,struct.pack('<I',retry))
    heading = index*7919 & 65535
    p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
    assert p.init()[1] is None
    plane = bytes(p.uc.mem_read(p.get(0x1c),width*height*4))
    lines = [' '.join(map(str,(width,height,*start,*goal,p.get(0xb0),heading,retry,p.get(0x54),8,budget)))]
    lines += [' '.join(str(p.get(0x90+i*4)) for i in range(8)),
              ' '.join(str(p.get(0x70+i*4)) for i in range(8)),
              ' '.join(str(p.get(i)) for i in (0xc0,0xc4,0xbc,0xc8,0xb4,0xb8))]
    lines += [f'{g} {plane[i*4]} {plane[i*4+1]}' for i,g in enumerate(grades)]
    queries, notifications, routes, finished = [], [], [], []
    def grade(uc,args):
        x,z,direction = struct.unpack('<iii',uc.mem_read(args,12))
        queries.extend((x,z,direction))
        return 3,grades[z*width+x] if 0<=x<width and 0<=z<height else 0
    def notify(uc,args):
        notifications.append(struct.unpack('<I',uc.mem_read(args,4))[0]); return 1,0
    def receive(uc,args):
        address,count = struct.unpack('<II',uc.mem_read(args,8))
        routes.append(struct.unpack('<'+'h'*(count*2),uc.mem_read(address,count*4)) if count else ())
        return 2,0
    def finish(uc,args):
        finished.append(True); return 0,0
    p.icd.hooks[0x4139d0] = grade
    p.icd.hooks[0x4e2470] = notify
    p.icd.hooks[0x4e4ea0] = receive
    p.icd.hooks[0x415f10] = finish
    p.uc.hook_add(UC_HOOK_CODE,lambda uc,a,s,d:uc.emu_stop(),begin=0x4166f1,end=0x4166f1)
    # Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
    p.icd.allow_early_stop=True
    expected = []
    phase = 1
    for _ in range(10000):
        queries.clear(); notifications.clear()
        p.uc.mem_write(OBJ+0x48,bytes(4))
        p.uc.mem_write(OBJ+0x165,struct.pack('<I',budget))
        result = 0
        if phase == 1:
            p.uc.mem_write(OBJ+0x5c,struct.pack('<I',2))
            _,error = p.icd.call(0x415b10,ecx=OBJ)
            assert error is None and p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
            phase = p.phase()
        else:
            for register,value in ((UC_X86_REG_EBP,STACK+STACK_SZ-2048),
                (UC_X86_REG_ESP,STACK+STACK_SZ-4096),(UC_X86_REG_ESI,OBJ),
                (UC_X86_REG_EBX,0),(UC_X86_REG_EAX,2),(UC_X86_REG_ECX,1),(UC_X86_REG_EDX,budget)):
                p.uc.reg_write(register,value)
            p.uc.emu_start(0x41665f,0x416952,timeout=5_000_000)
            eip = p.uc.reg_read(UC_X86_REG_EIP)
            assert eip in (0x4166f1,0x416952),hex(eip)
            if eip==0x4166f1: result=2
        if finished: result=1
        plane=bytes(p.uc.mem_read(p.get(0x1c),width*height*4))
        flags=struct.unpack('<I',p.uc.mem_read(unit+0x134,4))[0]&15
        route=routes[-1] if routes else ()
        values=(result,p.get(0x48),notifications[-1] if notifications else 0,p.get(0x40),
            digest(plane[i]|plane[i+1]<<8 for i in range(0,len(plane),4)),digest(queries),flags,len(route)//2,*route)
        expected.append(' '.join(map(str,values)))
        if result: break
    else: raise AssertionError('attempt did not terminate')
    return '\n'.join(lines)+'\n',expected+['END']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    total=0
    for index in range(13):
        for budget in (1,37,12000):
            for retry in (0,1,2,3):
                data,expected=case(index,budget,retry)
                actual=subprocess.run([args.runner,'--attempt'],input=data,text=True,capture_output=True,check=True).stdout.splitlines()
                assert actual==expected,(index,budget,retry,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected))))
                total+=len(expected)-1
    print(f'PASS: 156 search attempts, {total} boundaries; handoff, work, queries, cell plane and delivered routes match')

if __name__=='__main__': main()
