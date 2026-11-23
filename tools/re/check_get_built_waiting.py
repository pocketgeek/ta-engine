#!/usr/bin/env python3
"""Compare unfinished GetBuilt scheduling; weapon and decay callbacks observed."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_construction_test')
    args=ap.parse_args();p=Icd()
    unit,mission,game=HEAP,HEAP+0x1000,HEAP+0x10000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v))
    word=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game);p.uc.mem_write(unit+0x108,struct.pack('<f',0.5))
    callbacks=[]
    def stop(uc,args):
        if word(args)!=3:raise AssertionError('weapon stop selector')
        callbacks.append('stop');return 1,0
    def decay(uc,args):
        if (word(args),word(args+4))!=(unit,1):raise AssertionError('decay arguments')
        callbacks.append('decay');return 2,0
    p.hooks[0x519b70]=stop;p.hooks[0x429d70]=decay;p.freeze_hooks()
    rng=random.Random(0x402220);rows=[];expected=[]
    for case in range(4096):
        tick=rng.getrandbits(32);stage=rng.randrange(5)
        events=rng.choice([0,1,0x10000000,0x10000001,rng.getrandbits(32)])
        wait,deadline=rng.getrandbits(32),rng.getrandbits(32)
        put(game+0x19f44,tick);p.uc.mem_write(mission+5,bytes([stage]))
        put(mission+6,wait);put(mission+10,deadline);callbacks.clear()
        signed_events=struct.unpack('<i',struct.pack('<I',events))[0]
        result,error=p.call(0x402220,(unit,mission,signed_events))
        if error:raise RuntimeError(error)
        expected.append([result,p.uc.mem_read(mission+5,1)[0],word(mission+6),word(mission+10),callbacks.count('stop'),callbacks.count('decay')])
        rows.append(f'{tick} {stage} {events} {wait} {deadline}')
    result=subprocess.run([args.binary,'--get-built'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print('PASS: 4096 unfinished GetBuilt timers, events and callbacks')


if __name__=='__main__':main()
