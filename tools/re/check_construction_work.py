#!/usr/bin/env python3
"""Compare construction payment/progress with 429af0; lifecycle callbacks controlled."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_construction_test')
    args=ap.parse_args();p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    builder,site,owner,resources,kind=[HEAP+n*0x10000 for n in range(5)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    real=lambda a,v:p.uc.mem_write(a,struct.pack('<f',v))
    put(builder+0xb8,owner);put(owner+0x10c,resources);put(site+0xb4,kind)
    events=[]
    p.hooks[0x429990]=lambda uc,sp:(events.append(2) or (3,0))
    p.hooks[0x51a140]=lambda uc,sp:(events.append(3) or (5,0))
    p.freeze_hooks()
    rng=random.Random(0x429af0);rows=[];expected=[]
    f32=lambda v:struct.unpack('<f',struct.pack('<f',v))[0]
    for case in range(8192):
        remaining=f32(rng.choice([0.,1.,0.00001,rng.random()]))
        hp=rng.randrange(65536);event=rng.getrandbits(32);flags=rng.getrandbits(32)
        inverse=f32(rng.choice([0.001,0.01,0.1,1./3000]));cost=f32(rng.randrange(10000))
        maxhp=rng.randrange(1,30001);stored=f32(rng.choice([0.,0.001,rng.random()*1000]))
        allocation=f32(rng.choice([0.,0.01,0.5,1.,rng.random()]))
        requested=f32(rng.random()*100);produced=f32(rng.random()*100)
        total=rng.random()*1e6;work=f32(rng.choice([0.,-0.1,-10.,0.1,10.,rng.random()*100]))
        real(site+0x108,remaining);put(site+0x10c,hp);put(site+0xd0,event);put(site+0x130,flags)
        real(kind+0x216,inverse);real(kind+0x20e,cost);put(kind+0x1be,maxhp)
        real(resources,stored);real(resources+8,allocation);real(resources+0x10,requested)
        real(resources+0xc,produced);p.uc.mem_write(resources+0x18,struct.pack('<d',total))
        events.clear()
        work_bits=struct.unpack('<i',struct.pack('<f',work))[0]
        result,error=p.call(0x429af0,(builder,site,work_bits))
        if error:raise RuntimeError(error)
        outcome=events[0] if events else int(bool(result))
        expected.append([outcome,get(site+0x108),get(site+0x10c)&65535,get(site+0xd0),get(site+0x130),
                         get(resources),get(resources+0x10),get(resources+0xc),
                         struct.unpack('<Q',p.uc.mem_read(resources+0x18,8))[0]])
        rows.append(' '.join(map(str,[remaining,hp,event,flags,inverse,cost,maxhp,stored,
                                     allocation,requested,produced,total,work])))
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,row.split())) for row in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} construction/unconjure updates, resource accounting and lifecycle outcomes')


if __name__=='__main__':main()
