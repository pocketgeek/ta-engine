#!/usr/bin/env python3
"""Compare ballistic animation elevation against native 52bd10."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();game,weapon,vector=[HEAP+i*0x20000 for i in range(3)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def float32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
put(0x62d55c,game);rng=random.Random(0x52bd10);rows=[];expected=[]
for i in range(8192):
    x,y,z=[float32(rng.uniform(-1000,1000)) for _ in range(3)]
    if i%31==0:x=z=0
    speed=float32(rng.uniform(.1,60));scale=float32(rng.uniform(.05,4))
    high=i%2;gravity=rng.randrange(1,20000)
    p.uc.mem_write(vector,struct.pack('<3f',x,y,z));put(weapon+4,high)
    p.uc.mem_write(weapon+8,struct.pack('<f',scale));put(game+0x19ecc,gravity)
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    value,error=p.call(0x52bd10,(vector,struct.unpack('<I',struct.pack('<f',speed))[0]),ecx=weapon)
    if error:raise RuntimeError(error)
    expected.append(value&65535);rows.append(f'{x} {y} {z} {speed} {scale} {high} {gravity}')
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--ballistic-aim'],
                      input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=list(map(int,result.stdout.split()))
assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 8192 native ballistic elevations, including both arc preferences and unreachable targets')
