#!/usr/bin/env python3
"""Compare integer COB motion commands and piece updates with the original VM."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_script_test')
    args=parser.parse_args()
    p=Icd(); vm,descriptor,code,pieces,vtable=[HEAP+n*0x10000 for n in range(5)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    pose=[0]*6
    def read_pose(base):
        return lambda uc,sp:(2,pose[base+get(sp+4)])
    def write_pose(base):
        def call(uc,sp):
            pose[base+get(sp+4)]=get(sp+8)
            return 3,0
        return call
    for offset,address,callback in ((0,0x56a000,write_pose(0)),(4,0x56a010,write_pose(3)),
                                    (24,0x56a020,read_pose(0)),(28,0x56a030,read_pose(3))):
        put(vtable+offset,address); p.hooks[address]=callback
    p.freeze_hooks()
    rng=random.Random(0x56d850); rows=[]; expected=[]
    for _ in range(6000):
        op=rng.choice([0,0x10001000,0x10002000,0x10003000,0x10004000,0x1000b000,0x1000c000])
        axis=rng.randrange(3); target=rng.randrange(-10000000,10000000)
        speed=rng.randrange(200000); elapsed=rng.choice([0,1,2,5,30])
        motion=[rng.randrange(-1000000,1000000) for _ in range(6)]
        motion += [rng.choice([-1,rng.randrange(65536)]) for _ in range(3)]
        motion += [rng.randrange(-3000,3000) for _ in range(9)]
        pose[:]=[rng.randrange(-10000000,10000000)&0xffffffff for _ in range(3)]+[rng.randrange(65536) for _ in range(3)]
        active=rng.randrange(2)
        signed=lambda x:struct.unpack('<i',struct.pack('<I',x&0xffffffff))[0]
        rows.append(' '.join(map(str,[op,axis,target,speed,elapsed,*motion,*map(signed,pose),active])))
        p.uc.mem_write(vm,bytes(0xa64)); put(vm,vtable); put(vm+4,30); put(vm+0xc,descriptor)
        put(vm+0x18,pieces); put(vm+0x1c,1); put(descriptor+8,1); put(descriptor+0x24,code)
        put(pieces,active)
        for n,value in enumerate(motion): put(pieces+4+n*4,value)
        if op:
            program=[]
            if op in (0x10001000,0x10002000,0x10003000): program += [0x10021001,speed]
            program += [0x10021001,target&0xffffffff,op,0,axis,0x10021001,0,0x10013000]
            p.uc.mem_write(code,struct.pack(f'<{len(program)}I',*program))
            put(vm+0x20,0x1000000); put(vm+0x28,0xffffffff)
            _,error=p.call(0x56c8b0,(0,0),ecx=vm)
            if error: raise RuntimeError(error)
        _,error=p.call(0x56d850,(elapsed,),ecx=vm)
        if error: raise RuntimeError(error)
        expected.append([*struct.unpack('<18i',p.uc.mem_read(pieces+4,72)),*map(signed,pose),get(pieces)])
    proc=subprocess.run([args.binary,'--pieces'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,row.split())) for row in proc.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError('row count mismatch')
    for fixture,want,got in zip(rows,expected,actual):
        if want!=got: raise AssertionError((fixture,want,got))
    print(f'PASS: {len(rows)} original COB motion commands and integer piece updates')


if __name__=='__main__': main()
