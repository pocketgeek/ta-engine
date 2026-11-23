#!/usr/bin/env python3
"""Compare raw terrain grades with retail on feature-free map cells."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_trace_test')
    args=ap.parse_args()
    p=Icd();game,grid,cells=HEAP,HEAP+0x20000,HEAP+0x30000
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v))
    put(0x62d55c,game);put(game+0x19e98,8);put(game+0x19e9c,8);put(game+0x19f04,cells)
    rng=random.Random(0x508bc1);rows=[];expected=[]
    for i in range(16000):
        low,high=sorted([rng.randrange(256),rng.randrange(256)])
        sea=rng.randrange(256)
        minDepth,maxDepth=sorted([rng.randrange(-300,301),rng.randrange(-300,301)])
        badMin,badMax=sorted([rng.randrange(minDepth,maxDepth+1),rng.randrange(minDepth,maxDepth+1)])
        maxSlope,maxWater=rng.randrange(256),rng.randrange(256)
        badSlope,badWater=rng.randrange(maxSlope+1),rng.randrange(maxWater+1)
        if i%4==0: minDepth,badMin,maxDepth,badMax=-10000,-10000,10000,10000
        road=i%2
        record=bytearray(14);record[5]=high;record[6]=low;record[13]=road*128
        struct.pack_into('<H',record,8,65535)
        p.uc.mem_write(cells+(2*8+2)*14,bytes(record));p.uc.mem_write(game+0x19ef8,bytes([sea]))
        p.uc.mem_write(grid+8,struct.pack('<4h4B',maxDepth,minDepth,badMax,badMin,maxSlope,badSlope,maxWater,badWater))
        value,error=p.call(0x5088f0,(grid,2,2,1,1))
        if error:raise RuntimeError(error)
        rows.append(' '.join(map(str,[low,high,sea,maxDepth,minDepth,badMax,badMin,maxSlope,badSlope,maxWater,badWater,road])))
        expected.append(value)
    proc=subprocess.run([args.binary,'--terrain-grade'],input='\n'.join(rows)+'\n',capture_output=True,text=True,check=True)
    actual=list(map(int,proc.stdout.split()))
    if actual!=expected:
        for i,(want,got) in enumerate(zip(expected,actual)):
            if want!=got:raise AssertionError((i,rows[i],want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} terrain grades, hard/soft slopes, depth bands and road exceptions')


if __name__=='__main__':main()
