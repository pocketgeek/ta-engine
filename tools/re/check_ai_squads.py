#!/usr/bin/env python3
"""Compare complete empty AI squad handlers; no planner routines are substituted."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_ai_test')
    args=ap.parse_args();p=Icd()
    game,player,manager,groups,squad=HEAP,HEAP+0x20000,HEAP+0x30000,HEAP+0x40000,HEAP+0x50000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);put(player+0x84,groups);put(manager,player)
    put(squad+4,manager);put(squad+8,groups+196)
    data=groups+196;put(data+4,1)
    rng=random.Random(0x40e180);rows=[];expected=[]
    handlers=[0x40b320,0x40e180,0x40ea30,0x40eb40,0x4101e0]
    p.freeze_hooks()
    for i in range(4096):
        kind=i%5; tick=rng.choice([0,10084,0xfffffff0,rng.getrandbits(32)])
        seed=rng.getrandbits(32)
        target=rng.randrange(100) if kind in (0,3,4) else 0
        parameter=rng.randrange(-2000000,2000000);active=rng.randrange(3);dirty=rng.randrange(2)
        put(game+0x19f44,tick);put(0x64186c,seed)
        put(data+8,active);put(data+12,target);put(data+32,parameter);put(data+0xb0,dirty)
        _,error=p.call(handlers[kind],ecx=squad)
        if error:raise RuntimeError(error)
        end_parameter=struct.unpack('<i',struct.pack('<I',get(data+32)))[0]
        expected.append([get(squad+12),get(0x64186c),get(data+12),end_parameter,get(data+8),get(data+0xb0)])
        rows.append(' '.join(map(str,[kind,tick,seed,target,parameter,active,dirty])))
    proc=subprocess.run([args.binary,'--empty'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} complete empty base/strike/backup/VTOL/reserve squad updates')


if __name__=='__main__':main()
