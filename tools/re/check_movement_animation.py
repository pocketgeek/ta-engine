#!/usr/bin/env python3
"""Compare render snapshot animation suppression with retail's actual GET 29.

The original query (50ceb0 -> 4dc100) runs with controlled mover inputs. Cardinal
velocities and integral percentages isolate refusal/attachment gating from the
separate velocity-vector normalization and rounding behavior.
"""
import argparse
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_visual_test')
    args=ap.parse_args()
    p=Icd();vm,model,unit,mover,kind=[HEAP+i*0x10000 for i in range(5)]
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    put(vm+0xa64,model);put(model+12,unit);put(unit+8,mover);put(unit+0xb4,kind)
    rows=[];expected=[]
    for blocked in (0,1,2):
        for attached in (0,1):
            for speed,maximum in ((0,5),(1,5),(1,2),(1,1),(2,1)):
                p.uc.mem_write(mover+0x36,struct.pack('<H',(0,8,4)[blocked]))
                put(unit+0xa8,attached);put(unit+0x12b,maximum*65536)
                for dx,dz in ((speed,0),(-speed,0),(0,speed),(0,-speed)):
                    put(mover+8,dx*65536);put(mover+16,dz*65536)
                    native,error=p.call(0x50ceb0,(29,0,0,0,0),ecx=vm)
                    if error:raise RuntimeError(error)
                    rows.append(f'{blocked} {attached} {speed*65536} {maximum*65536}\n')
                    expected.append((native,int(native>5)))
    result=subprocess.run([args.binary,'--movement-animation'],input=''.join(rows),text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    if actual!=expected:
        index=next(i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b)
        raise AssertionError((rows[index],expected[index],actual[index]))
    print(f'PASS: {len(expected)} native GET 29/render snapshot cases; moving, first refusal, repeated refusal, attachment and four headings')


if __name__=='__main__':main()
