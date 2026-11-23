#!/usr/bin/env python3
"""Compare World gate ownership grading with native grade/capability/map queries.

The cached grade and visibility are controlled inputs. The native capability,
ordered gate probes and owner interpretation execute without substitution.
"""
import argparse
import itertools
import struct
import subprocess

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_trace_test')
    args=parser.parse_args()
    p=Icd()
    game,obj,grid,plane,pool,kind,requester,cells,ai,config,options,debug,player=(
        HEAP+n for n in (0,0x20000,0x21000,0x22000,0x24000,0x25000,0x26000,
                         0x28000,0x2d000,0x2e000,0x2f000,0x30000,0x31000))
    gate=pool+312
    def put(a,v): p.uc.mem_write(a,struct.pack('<I',v))
    put(0x62d55c,game);put(0x62d558,config)
    put(config,options);put(config+4,debug);put(ai,player)
    put(game+0x2484,ai)
    put(game+0x19e98,32);put(game+0x19e9c,32);put(game+0x19f04,cells)
    put(game+0x14e84,pool);put(game+0x14e88,gate)
    put(gate+0x130,0x1000001);put(gate+0xb4,kind);put(kind+0x264,0x40000000)
    put(obj+0x6c,grid);put(obj+0x58,requester)
    put(grid+0x340,32);put(grid+0x344,32);put(grid+0x348,plane)
    p.uc.mem_write(plane,bytes([0x33])*512)
    p.hooks[0x413c80]=lambda uc,a:(4,1)
    rows,expected=[],[]
    previous=None
    for fx,fz,retry,capable,owner,opened,frame,dx,dz in itertools.product(
            (1,2,3),(1,2,3),(0,1,2),(0,1),(0,1),(0,1),(0,1),range(-1,4),range(-1,4)):
        p.uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        p.uc.mem_write(requester+0x78,struct.pack('<hh',fx,fz))
        put(obj+0x1ad,retry)
        p.uc.mem_write(ai+0x1a5,bytes([capable]));p.uc.mem_write(gate+0xfd,bytes([owner]))
        if previous is not None: p.uc.mem_write(previous,bytes(14))
        previous=cells+((10+dz)*32+10+dx)*14
        record=bytearray(14)
        struct.pack_into('<H',record,0,int(not opened or frame))
        record[13]=0 if frame else 32
        p.uc.mem_write(previous,bytes(record))
        value,error=p.call(0x4139d0,(10,10,0),ecx=obj)
        assert error is None,error
        assert p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000
        rows.append(' '.join(map(str,(fx,fz,retry,capable,owner,opened,frame,dx,dz))))
        expected.append(value)
    result=subprocess.run([args.binary,'--gate-owner'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=list(map(int,result.stdout.split()))
    assert len(actual)==len(expected),(len(actual),len(expected))
    for row,want,got in zip(rows,expected,actual):
        assert got==want,(row,'native',want,'World',got)
    print(f'PASS: {len(rows)} World/native gate owner grades, capability, retries and square/rectangular footprints')


if __name__=='__main__':
    main()
