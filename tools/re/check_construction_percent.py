#!/usr/bin/env python3
"""Compare COB BUILD_PERCENT_LEFT with the executable's unit getter."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_construction_test')
    args=ap.parse_args();p=Icd()
    p.uc.mem_write(HEAP+0xa64,struct.pack('<I',HEAP+0x10000))
    p.uc.mem_write(HEAP+0x1000c,struct.pack('<I',HEAP+0x20000))
    rng=random.Random(0x50d1ab)
    values=[0,1,0.000001,0.01,0.5,0.999]+[rng.random() for _ in range(8192)]
    expected=[]
    for value in values:
        p.uc.mem_write(HEAP+0x20108,struct.pack('<f',value))
        result,error=p.call(0x50ceb0,(17,0,0,0,0),ecx=HEAP)
        if error:raise RuntimeError(error)
        expected.append(result)
    proc=subprocess.run([args.binary,'--percent'],input='\n'.join(map(str,values))+'\n',
                        text=True,capture_output=True,check=True)
    actual=list(map(int,proc.stdout.split()))
    if actual!=expected:raise AssertionError(next((v,a,b) for v,a,b in zip(values,actual,expected) if a!=b))
    print(f'PASS: {len(values)} original construction-percent script values')


if __name__=='__main__':main()
