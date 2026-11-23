#!/usr/bin/env python3
"""Compare ground PARK scheduling/ring requests; navigator installation observed."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_mission_test')
    args=ap.parse_args();p=Icd()
    unit,mission,target,kind,target_kind=[HEAP+i*0x1000 for i in range(5)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    word=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    calls=[]
    def clear(uc,argv):
        if word(argv):raise AssertionError('nonzero cleared target')
        put(uc.reg_read(UC_X86_REG_ECX)+4,0);calls.append(('clear',));return 1,0
    def reset(uc,argv):
        center,outer,inner=struct.unpack('<Iii',uc.mem_read(argv,12))
        if center not in (unit+0x68,target+0x68):raise AssertionError('ring center')
        calls.append(('reset',int(center==target+0x68),outer,inner));return 3,0
    p.hooks[0x5199f0]=clear;p.hooks[0x4d4e10]=reset;p.freeze_hooks()
    rng=random.Random(0x407ca0);rows=[];expected=[]
    for case in range(8192):
        seed,mask=rng.getrandbits(32),rng.getrandbits(32)
        stage=rng.randrange(4);events=rng.choice([0,0,8,0x100,0x2700,rng.getrandbits(32)])
        mover,present,valid,next_ready=[rng.randrange(2) for _ in range(4)]
        foot,tx,tz=[rng.randrange(1,33) for _ in range(3)]
        padding=rng.randrange(4096);permanent=rng.choice([0,1,-1]);attempts=rng.randrange(-4,16)&0xffffffff
        put(0x64186c,seed);put(unit+8,HEAP+0x6000 if mover else 0)
        put(unit+0xb4,kind);put(kind+0x260,0);put(unit+0x130,0x1000020)
        put(unit+0x108,0);put(unit+0x104,0);put(unit+0xa8,0)
        p.uc.mem_write(unit+0x78,struct.pack('<hh',foot,foot))
        put(target+0x130,0x1000000 if valid else 0);put(target+0xb4,target_kind)
        p.uc.mem_write(target_kind+0x126,struct.pack('<hh',tx,tz))
        put(mission+0x16,target if present else 0);put(mission+0xe,unit)
        p.uc.mem_write(mission+5,bytes([stage]));put(mission+6,mask)
        put(mission+0x4e,padding);put(mission+0x52,permanent);put(mission+0x56,attempts)
        put(mission+0x66,HEAP+0x7000 if next_ready else 0);calls.clear()
        signed_events=struct.unpack('<i',struct.pack('<I',events))[0]
        result,error=p.call(0x407ca0,(unit,mission,signed_events))
        if error:raise RuntimeError(error)
        resets=[c for c in calls if c[0]=='reset']
        expected.append([result,p.uc.mem_read(mission+5,1)[0],word(mission+6),word(mission+0x56),
            word(0x64186c),sum(c[0]=='clear' for c in calls),len(resets),*(resets[0][1:] if resets else (0,0,0))])
        rows.append(' '.join(map(str,[seed,stage,mask,events,mover,present,valid,next_ready,foot,tx,tz,padding,permanent,attempts])))
    proc=subprocess.run([args.binary,'--park'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print('PASS: 8192 original ground PARK stages, target loss, retry RNG and ring requests')


if __name__=='__main__':main()
