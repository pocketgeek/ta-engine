#!/usr/bin/env python3
"""Compare ordinary VTOL_LandIfCan stages using explicit controlled host inputs."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_mission_test')
    args=ap.parse_args();p=Icd()
    unit,mission,game,controller,kind,mover=[HEAP+n*0x30000 for n in range(6)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);put(unit+0xa4,1);put(unit+0xb4,kind)
    p.hooks[0x51d1e0]=lambda uc,a:(3,0)
    p.hooks[0x4eb9e0]=lambda uc,a:(0,controller)
    p.hooks[0x4ea3f0]=lambda uc,a:(2,0)
    rng=random.Random(0x416cd0);rows=[];expected=[]
    for i in range(2048):
        position=[rng.randrange(-1000000000,1000000000) for _ in range(3)]
        anchor=[rng.randrange(-1000000000,1000000000) for _ in range(3)] if i%2 else [0,0,0]
        heading,fx,fz=rng.randrange(65536),rng.randrange(1,16),rng.randrange(1,16)
        events=rng.choice([0,0x100,0x200,0x400,0x700]);stage=i%5
        mask,angle,parity,seed=[rng.randrange(2**32) for _ in range(4)]
        canceled=i%17==0;mover_on=i%7!=0;fly=i%11!=0;can_transport=i%2==0
        velocity=rng.randrange(101);height=rng.randrange(600);accept=(i//5)%15-1
        p.uc.mem_write(unit+0x68,struct.pack('<3i',*position));p.uc.mem_write(unit+0x78,struct.pack('<2H',fx,fz))
        p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
        p.uc.mem_write(mission+0x22,struct.pack('<3i',*anchor));p.uc.mem_write(mission+5,bytes([stage]))
        for off,value in [(6,mask),(0x4e,angle),(0x52,parity),(0x66,int(canceled))]:put(mission+off,value)
        put(0x64186c,seed);put(unit+8,mover if mover_on else 0);put(kind+0x260,0x800 if fly else 0)
        put(kind+0x264,0x200 if can_transport else 0)
        calls=[0,0,0,0,0];goal=[];goals=[];callback_names=[]
        def initialize(uc,a):calls[1]+=1;return 4,0
        def landable(uc,a):
            good=calls[0]==accept;calls[0]+=1;return 2,int(good)
        def touchdown(uc,a):
            pointer=struct.unpack('<I',uc.mem_read(a,4))[0]
            name=bytes(uc.mem_read(pointer,80)).split(b'\0')[0].decode('ascii')
            callback_names.append(name)
            if name=='BeginLanding':calls[2]+=1
            return 3,0
        def deactivate(uc,a):calls[3]+=1;return 2,0
        def finish(uc,a):calls[4]+=1;return 2,0
        def create(uc,a):goal[:]=[*struct.unpack('<3i',uc.mem_read(get(a+4),12)),0x20,0];return 2,controller
        def radius(uc,a):goal[3]|=0x10;goal[4]=get(a);return 1,0
        def altitude(uc,a):goal[3]|=8;goal[1]=min(511,max(height,0)+struct.unpack('<i',uc.mem_read(a,4))[0])*65536;return 1,0
        def install(uc,a):goals.append(list(goal));return 1,0
        p.hooks[0x416c50]=initialize;p.hooks[0x509400]=landable
        p.hooks[0x4dc100]=lambda uc,a:(1,velocity)
        p.hooks[0x511170]=lambda uc,a:(1,height)
        p.hooks[0x56c5c0]=touchdown;p.hooks[0x51e4d0]=deactivate;p.hooks[0x4da750]=finish
        p.hooks[0x4e40e0]=create;p.hooks[0x4e4540]=radius;p.hooks[0x4e44c0]=altitude;p.hooks[0x4d4d40]=install
        result,error=p.call(0x416cd0,(unit,mission,events))
        if error:raise RuntimeError(error)
        expected_names=(['EndTransport'] if can_transport and calls[2] else [])+(['BeginLanding'] if calls[2] else [])
        if callback_names!=expected_names:
            raise AssertionError((i,callback_names,expected_names))
        rows.append(' '.join(map(str,[*position,*anchor,heading,fx,fz,events,stage,mask,angle,parity,int(canceled),int(mover_on),int(fly),seed,velocity,height,accept])))
        expected.append([result,p.uc.mem_read(mission+5,1)[0],get(mission+6),get(mission+0x4e),get(mission+0x52),get(0x64186c),*struct.unpack('<3i',p.uc.mem_read(mission+0x22,12)),*calls,len(goals),*[v for g in goals for v in g]])
    proc=subprocess.run([args.binary,'--landing'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if len(actual)!=len(expected):raise AssertionError('row count')
    for row,want,got in zip(rows,expected,actual):
        if want!=got:raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} landing mission stages, goals, callbacks and RNG boundaries')


if __name__=='__main__':main()
