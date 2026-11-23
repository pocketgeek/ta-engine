#!/usr/bin/env python3
"""Compare retained flight acceleration and body attitude with native 4da620."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd()
unit,mover,kind,game,delta=[HEAP+i*0x10000 for i in range(5)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(0x62d55c,game);put(unit+0xb4,kind)
rng=random.Random(0x4da620);rows=[];expected=[]
for i in range(8192):
    retained=[rng.randrange(-40*65536,40*65536) for _ in range(3)]
    acceleration=[rng.randrange(-4*65536,4*65536) for _ in range(3)]
    heading=rng.randrange(65536)
    bank,pitch=[rng.randrange(-3*65536,3*65536) for _ in range(2)]
    gravity=rng.randrange(1,30000)
    p.uc.mem_write(mover+0x14,struct.pack('<3i',*retained))
    p.uc.mem_write(delta,struct.pack('<3i',*acceleration))
    p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
    put(kind+0x176,bank);put(kind+0x17a,pitch);put(game+0x19ecc,gravity)
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    _,error=p.call(0x4da620,(unit,delta),ecx=mover)
    if error:raise RuntimeError(error)
    expected.append((*struct.unpack('<3i',p.uc.mem_read(mover+0x14,12)),
                     struct.unpack('<H',p.uc.mem_read(unit+0x7c,2))[0],
                     struct.unpack('<H',p.uc.mem_read(unit+0x80,2))[0]))
    rows.append(' '.join(map(str,[*retained,*acceleration,heading,bank,pitch,gravity])))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--flight-attitude'],
                      input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 8192 native flight acceleration/roll/pitch updates')
