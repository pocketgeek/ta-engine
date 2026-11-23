#!/usr/bin/env python3
"""Check player bookkeeping's clock/RNG; cache rebuild bodies are controlled."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args(); p=Icd(); game,manager=HEAP,HEAP+0x30000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game)
    p.hooks[0x411700]=lambda uc,a:(0,0)
    p.hooks[0x4119d0]=lambda uc,a:(0,0)
    rng=random.Random(0x411a90);rows=[];expected=[]
    for i in range(4096):
        enabled=i%7!=0
        last=rng.choice([0,10069,0xfffffff9,0xfffffffa,0xffffffff,rng.getrandbits(32)])
        tick=(last+rng.randrange(5,10))&0xffffffff
        seed=rng.getrandbits(32);refresh=[]
        p.hooks[0x410810]=lambda uc,a:(refresh.append(True) or (0,0))
        put(0x62a33c,manager if enabled else 0);put(game+0x19f44,tick)
        put(manager+0x101,last);put(0x64186c,seed)
        _,error=p.call(0x4121c0,(0,))
        if error:raise RuntimeError(error)
        # b uses an unsigned input timestamp encoded through scanf's int slot.
        signed_last=struct.unpack('<i',struct.pack('<I',last))[0]
        rows.append(f'b {int(enabled)} {signed_last} {tick} {seed}')
        expected.append([get(manager+0x101),get(0x64186c),int(bool(refresh))])
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} player-cache deadlines, wrap boundaries, rebuild choices and RNG states')


if __name__=='__main__':main()
