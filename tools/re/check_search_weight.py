#!/usr/bin/env python3
"""Verify age/load initial search weight using real 4161b0 instructions."""
import argparse
import itertools
import struct
import subprocess
from emuphase import Phase, OBJ, GS, TYPE
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    p=Phase(); p.unit(2,2); assert p.construct() is None
    p.plant_request(p.U,(2,2),(20,20))
    data=[]; expected=[]
    for (tick,last),pending,retry,heavy,threshold in itertools.product(
            ((100,0),(100,100),(100,94),(100,93),(100,92),(1000,1),(2,0xfffffff0),(3,4)),
            (0,19,20,21,100,500),(0,1,2,3),(0,1),(0,20,50)):
        for address,value in ((GS+0x19f44,tick),(OBJ+0x169,last),(OBJ+0x1ad,retry),
                              (0x634674,pending),(0x60523c,threshold),(TYPE+0x260,heavy*0x80000)):
            p.uc.mem_write(address,struct.pack('<I',value))
        p.uc.mem_write(TYPE+0x194,struct.pack('<h',heavy))
        out=OBJ+0x380
        _,error=p.icd.call(0x4161b0,(out,),ecx=OBJ)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        value=struct.unpack('<i',p.uc.mem_read(out,4))[0]
        if value<65536 or value>20*65536: value=20*65536
        stamp=struct.unpack('<I',p.uc.mem_read(OBJ+0x169,4))[0]
        data.append(' '.join(map(str,(tick,last,pending,retry,heavy,threshold))))
        expected.append(f'{value} {stamp}')
    actual=subprocess.run([args.runner,'--weight'],input='\n'.join(data)+'\n',text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,data[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected)))
    print(f'PASS: {len(expected)} initial weights and wrap timestamps match retail')

if __name__=='__main__': main()
