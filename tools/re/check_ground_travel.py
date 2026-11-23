#!/usr/bin/env python3
"""Compare full original active steering with World's production adapter.

Controlled navigator points/active/formation queries and discarded COB
notifications are the only substitutions. No copied native movement tables.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args();p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    unit,mover,kind,nav,vt=HEAP,HEAP+0x1000,HEAP+0x2000,HEAP+0x3000,HEAP+0x4000
    def put(fmt,address,*values): p.uc.mem_write(address,struct.pack('<'+fmt,*values))
    def get(fmt,address): return struct.unpack('<'+fmt,p.uc.mem_read(address,struct.calcsize('<'+fmt)))
    put('I',unit+8,mover);put('I',unit+0xb4,kind);put('I',mover,nav);put('I',nav,vt)
    active,query,formation=HEAP+0x5000,HEAP+0x5004,HEAP+0x5008
    put('I',vt+0x14,active);put('I',vt+0xc,query);put('I',vt+0x34,formation)
    p.hooks[active]=lambda uc,a:(0,1);p.hooks[formation]=lambda uc,a:(0,0)
    p.hooks[0x56c640]=lambda uc,a:(8,0)
    points=[]
    def supply_points(uc,a):
        output,count=get('2I',a)
        if count not in (2,3): raise AssertionError(count)
        put('i'*count*3,output,*(v for x,z in points[:count] for v in (x,0,z)))
        return 2,output
    p.hooks[query]=supply_points;p.freeze_hooks()
    rng=random.Random(0x4d9ad0);rows=[];expected=[]
    for i in range(16000):
        speed,maximum,accel,brake=[rng.randrange(0,8*65536) for _ in range(4)]
        rate=rng.choice([0,1,180,500,2300,9000,32768,65535])
        road,water=rng.choice([65536,78643,98304,131072]),rng.choice([0,32768,65536,78643])
        pitch=rng.randrange(65536);speed_mode=i%8;heading=rng.randrange(65536)
        flags=rng.choice([0,0x800,0x1000,0x1800]);mode=i//8%8
        points=[(rng.randrange(-300*65536,300*65536),rng.randrange(-300*65536,300*65536)) for _ in range(3)]
        if i%17==0: points[1]=(0,0)
        if i%19==0: points[2]=points[1]
        if i%23==0: points[0]=points[1]
        put('i',unit+0x12b,maximum);put('i',kind+0x162,maximum)
        put('ii',kind+0x166,brake,accel);put('H',kind+0x18e,rate)
        put('ii',kind+0x16e,water,road);put('i',mover+0x20,speed)
        put('H',mover+0x36,1|flags|(speed_mode<<8)|(mode<<5))
        put('HH',unit+0x7e,heading,pitch)
        _,error=p.call(0x4d9ad0,(unit,),ecx=mover)
        if error: raise AssertionError((i,error))
        vx,vy,vz=get('3i',mover+8)
        if vy: raise AssertionError(('vertical displacement',vy))
        expected.append([get('H',unit+0x7e)[0],get('i',mover+0x20)[0],vx,vz])
        rows.append(' '.join(map(str,[speed,maximum,accel,brake,rate,road,water,pitch,speed_mode,
                                      heading,flags,mode]+[v for point in points for v in point])))
    result=subprocess.run([args.binary,'--ground-travel'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    for i,(row,want,got) in enumerate(zip(rows,expected,actual)):
        if want!=got: raise AssertionError((i,row,'retail',want,'World',got))
    print(f'PASS: {len(rows)} original active steering/speed/velocity updates against World')


if __name__=='__main__': main()
