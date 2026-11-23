#!/usr/bin/env python3
"""Observe retail area-clear selection, comparing the production selector.
Eligibility/visibility are fixture inputs; native rectangle scan, fixed distance
and tie behavior execute unmodified from the user's retail installation.
"""
import argparse,random,struct,subprocess
from emu import Icd,HEAP

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-o2/reclaimarea_test')
    args=ap.parse_args()
    p=Icd();u=HEAP;definition=u+0x1000;low=u+0x2000;high=low+16;out=high+16
    state=u+0x10000;settings=u+0x40000;settings_data=settings+0x100
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def get(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(u+0xb4,definition);put(definition+0x264,0x1000)
    put(0x62d55c,state);put(0x62d558,settings);put(settings+8,settings_data)
    p.uc.mem_write(settings_data+0x15,b'\0')
    active={}
    def lookup(uc,sp):
        point=get(sp);x=get(point);z=get(point+8)
        return 1,active.get((x,z),0)
    p.hooks[0x50e660]=lookup
    p.hooks[0x497100]=lambda uc,sp:(1,1)
    p.hooks[0x4223f0]=lambda uc,sp:(2,1)
    p.freeze_hooks()
    rng=random.Random(0x509cc0);rows=[];expected=[]
    for n in range(1024):
        x0=rng.randrange(16,800)*65536+rng.randrange(65536)
        z0=rng.randrange(16,800)*65536+rng.randrange(65536)
        nx=rng.randrange(1,6);nz=rng.randrange(1,6)
        x1=x0+(nx-1)*16*65536;z1=z0+(nz-1)*16*65536
        bx=x0+rng.randrange(-64,128)*65536+rng.randrange(65536)
        bz=z0+rng.randrange(-64,128)*65536+rng.randrange(65536)
        mask=rng.getrandbits(nx*nz);active.clear()
        for i in range(nx*nz):
            if mask>>i&1:active[(x0+i%nx*16*65536,z0+i//nx*16*65536)]=HEAP+0x80000+i*16
        put(u+0x68,bx);put(u+0x70,bz)
        for ptr,x,z in [(low,x0,z0),(high,x1,z1)]:
            put(ptr,x);put(ptr+4,0);put(ptr+8,z)
        result,error=p.call(0x509cc0,(u,low,high,out))
        if error:raise RuntimeError(error)
        expected.append((int(bool(result)),get(out) if result else 0,get(out+8) if result else 0))
        rows.append(f'{x0} {z0} {x1} {z1} {bx} {bz} {mask}')
    result=subprocess.run([args.binary,'--selector'],input='\n'.join(rows)+'\n',capture_output=True,text=True,check=True)
    actual=[tuple(map(int,l.split())) for l in result.stdout.splitlines()]
    assert actual==expected,next(((i,a,e) for i,(a,e) in enumerate(zip(actual,expected)) if a!=e),(len(actual),len(expected)))
    print('PASS: 1024 native area scans, fractional bounds/positions, empty cells and distance ties')
if __name__=='__main__':main()
