#!/usr/bin/env python3
"""Check animation terrain/flight occupancy and notification edges against retail."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-o2/retail_visual_test')
    args=ap.parse_args()
    p=Icd();unit,kind,game=[HEAP+i*0x10000 for i in range(3)]
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
    calls=[]
    def notify(uc,sp):
        values=struct.unpack('<8I',uc.mem_read(sp,32))
        assert values[:4]==(0x615c00,0,1,1),values
        calls.append(values[4])
        return 8,0
    p.hooks[0x56c640]=notify;p.freeze_hooks()
    put(unit+0xb4,kind);put(0x62d55c,game)
    rng=random.Random(0x4dc600);rows=[];expected=[]
    # Include the five-height shallow-water boundary, equality at the
    # model top / waterline, and state retention, then broad signed heights.
    cases=[(previous,mode,100+depth,100,line,top)
           for previous in range(6) for mode in range(4)
           for depth in range(-8,3) for line in (0,2,5,8)
           for top in (0,2,5,8)]
    cases.extend((rng.randrange(6),rng.randrange(256),rng.randrange(-32768,32768),
                  rng.randrange(256),rng.randrange(256),rng.randrange(-32768,32768))
                 for _ in range(4096))
    for previous,mode,height,sea,line,top in cases:
        put(unit+0x100,previous);put(unit+0x130,mode)
        p.uc.mem_write(unit+0x6e,struct.pack('<h',height))
        p.uc.mem_write(game+0x19ef8,bytes([sea]))
        p.uc.mem_write(kind+0x248,bytes([line]))
        p.uc.mem_write(kind+0x14c,struct.pack('<h',top))
        calls.clear()
        _,error=p.call(0x4dc600,(unit,))
        if error:raise RuntimeError(error)
        value=read(unit+0x100)
        assert calls==([] if value==previous else [value]),(previous,value,calls)
        expected.append(value)
        rows.append(' '.join(map(str,(previous,mode,height,sea,line,top))))
    result=subprocess.run([args.binary,'--animation-occupancy'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=list(map(int,result.stdout.split()))
    if actual!=expected:
        raise AssertionError(next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch'))
    print(f'PASS: {len(rows)} native occupancy states and notification edges')


if __name__=='__main__':main()
