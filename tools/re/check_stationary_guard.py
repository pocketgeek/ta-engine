#!/usr/bin/env python3
"""Compare Guard_NoMove timing with retail, observing its attack-scan call."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=parser.parse_args()
    p=Icd(); game,unit,mission=HEAP,HEAP+0x20000,HEAP+0x21000
    def put(address,value): p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    def get(address): return struct.unpack('<I',p.uc.mem_read(address,4))[0]
    scans=[]
    p.hooks[0x401a90]=lambda uc,sp:(scans.append(1) or 1,0)
    p.freeze_hooks(); put(0x62d55c,game)
    rng=random.Random(401); rows=[]; expected=[]
    for _ in range(4000):
        tick=rng.randrange(0x7fffffff); flags=rng.choice([0,2,8,10])
        stage=rng.randrange(256); mask=rng.randrange(65536)
        deadline=rng.randrange(0x100000000); seed=rng.randrange(1,0x7fffffff)
        rows.append(f'k {tick} {flags} {stage} {mask} {deadline} {seed}')
        put(game+0x19f44,tick); put(unit+0x130,flags<<24); put(0x64186c,seed)
        p.uc.mem_write(mission+5,bytes([stage])); put(mission+6,mask); put(mission+10,deadline)
        scans.clear(); result,error=p.call(0x401bb0,(unit,mission,0))
        if error: raise RuntimeError(error)
        expected.append([result,p.uc.mem_read(mission+5,1)[0],get(mission+6),get(mission+10),get(0x64186c),len(scans)])
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    assert len(actual)==len(expected)
    for row,want,got in zip(rows,expected,actual):
        if want!=got: raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} stationary guard timers, scans, mission states and RNG states')


if __name__=='__main__': main()
