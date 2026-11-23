#!/usr/bin/env python3
"""Compare ground-response stages against original Move_Ground.

Target selection, order validation/allocation and diversion are controlled host
inputs. This checks timers, response gates, attack/resume transitions and integer
leash distances; it does not certify target scoring or retaliation eligibility.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_mission_test')
    args=ap.parse_args(); p=Icd()
    unit,mission,game,kind,target,aux=[HEAP+n*0x30000 for n in range(6)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);put(unit+0xb4,kind)
    p.hooks[0x51d1e0]=lambda uc,a:(3,0)
    p.hooks[0x4eb9e0]=lambda uc,a:(0,aux)
    p.hooks[0x4d6c40]=lambda uc,a:(12,aux)
    p.hooks[0x4d6da0]=lambda uc,a:(0,0)
    p.hooks[0x4eba00]=lambda uc,a:(0,0)
    rng=random.Random(0x402b00); rows=[]; expected=[]
    for i in range(4096):
        tick=rng.getrandbits(32)
        seed,mask,deadline,pending,flags,radius=[rng.getrandbits(32) for _ in range(6)]
        stage=i%5;events=rng.choice([0,1,0x100,0x200,0x400,0x2000,0x2700])
        foot=rng.randrange(1,16);blocked=i%13==0;mode=rng.choice([-2147483648,-1,0,1,2147483647])
        move=rng.randrange(4);choose=i%3!=0;valid=i%7!=0;created=i%11!=0;leash=rng.randrange(65536)
        px,pz,gx,gz,ox,oz,tx,tz=[rng.randrange(-32768,32768) for _ in range(8)]
        if i%4==0: ox,oz=px,pz
        if i%8==0: tx,tz=gx,gz
        put(game+0x19f44,tick);put(0x64186c,seed)
        put(unit+0xa8,1 if blocked else 0);put(unit+0x130,move<<16);put(unit+0xf8,0)
        p.uc.mem_write(unit+0x78,struct.pack('<h',foot));p.uc.mem_write(kind+0x232,struct.pack('<H',leash))
        for address,x,z in [(unit+0x68,px,pz),(target+0x68,tx,tz),(mission+0x22,gx,gz)]:
            p.uc.mem_write(address,struct.pack('<3i',x*65536,0,z*65536))
        p.uc.mem_write(mission+0x2e,struct.pack('<2h',ox,oz))
        p.uc.mem_write(mission+5,bytes([stage]));p.uc.mem_write(aux+4,bytes([created]))
        for off,value in [(6,mask),(10,deadline),(0x6a,pending),(0x5a,flags),(0x4e,radius),(0x52,mode)]:put(mission+off,value)
        calls=[0,0,0,0,0]
        def select(uc,a):calls[0]+=1;return 1,target if choose else 0
        def reset(uc,a):calls[1]+=1;return 2,0
        def clear(uc,a):calls[2]+=1;return 1,0
        def install(uc,a):calls[3]+=1;return 2,0
        def validate(uc,a):
            calls[4]+=1;dest=get(a);uc.mem_write(dest,bytes([valid]));return 6,dest
        p.hooks[0x4d8370]=select;p.hooks[0x4d4da0]=reset;p.hooks[0x4d4d40]=clear
        p.hooks[0x4d7750]=install;p.hooks[0x4de530]=validate
        result,error=p.call(0x402b00,(unit,mission,events))
        if error:raise RuntimeError((i,error))
        rows.append(' '.join(map(str,[tick,seed,stage,mask,deadline,pending,flags,radius,events,
                    foot,int(blocked),mode,move,int(choose),int(valid),int(created),leash,px,pz,gx,gz,ox,oz,tx,tz])))
        expected.append([result,p.uc.mem_read(mission+5,1)[0],get(mission+6),get(mission+10),
                         get(mission+0x6a),get(mission+0x5a),get(mission+0x4e),get(0x64186c),
                         *struct.unpack('<2h',p.uc.mem_read(mission+0x2e,4)),*calls])
    proc=subprocess.run([args.binary,'--ground-response'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if len(actual)!=len(expected):raise AssertionError('row count')
    for row,want,got in zip(rows,expected,actual):
        if want!=got:raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} ground response stages, timers, attack/return callbacks and RNG boundaries')


if __name__=='__main__':main()
