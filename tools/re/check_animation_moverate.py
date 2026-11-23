#!/usr/bin/env python3
"""Compare MoveRate tiers, terrain scaling and notification edges with retail."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-o2/retail_visual_test')
    args=ap.parse_args()
    p=Icd();unit,mover,kind=[HEAP+i*0x10000 for i in range(3)]
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def word(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
    calls=[]
    def notify(uc,sp):
        values=struct.unpack('<8I',uc.mem_read(sp,32))
        assert values[:4]==(0x615bf4,0,1,1),values
        calls.append(values[4]);return 8,0
    p.hooks[0x56c640]=notify;p.freeze_hooks()
    put(unit+8,mover);put(unit+0xb4,kind)
    rng=random.Random(0x4db350);rows=[];expected=[]
    for i in range(8192):
        speed=rng.randrange(0,1000001);turn=rng.randrange(-32768,32768)
        dx,dz=[rng.randrange(-1000000,1000001) for _ in range(2)]
        slow,fast=[rng.randrange(0,1000001) for _ in range(2)]
        road,water=[rng.randrange(0,4*65536) for _ in range(2)]
        flags=rng.choice((0,4,8,0x800,0x1000,0x1800,0x1804))
        attached=int(i%17==0);flying=i%2;previous=i%4
        if i%7==0:speed=0
        if i%11==0:turn=0
        if i%13==0:dx=dz=0
        if i%19==0:slow=fast=speed;flags=0 # equality boundaries
        put(unit+0xa8,attached);put(unit+0x130,previous<<2)
        put(mover+8,dx);put(mover+16,dz);put(mover+0x20,speed)
        word(mover+0x36,flags);word(mover+0x24,turn)
        put(kind+0x182,slow);put(kind+0x186,fast)
        put(kind+0x172,road);put(kind+0x16e,water);put(kind+0x260,0x800 if flying else 0)
        calls.clear()
        _,error=p.call(0x4db350,(unit,),ecx=mover)
        if error:raise RuntimeError(error)
        value=(struct.unpack('<I',p.uc.mem_read(unit+0x130,4))[0]>>2)&3
        assert calls==([] if value==previous else [value]),(previous,value,calls)
        expected.append(value)
        rows.append(' '.join(map(str,(speed,turn,dx,dz,slow,fast,road,water,flags,attached,flying))))
    result=subprocess.run([args.binary,'--animation-moverate'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=list(map(int,result.stdout.split()))
    if actual!=expected:
        raise AssertionError(next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch'))
    print(f'PASS: {len(rows)} native ground/air MoveRate tiers and notification edges')


if __name__=='__main__':main()
