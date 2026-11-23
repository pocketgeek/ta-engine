#!/usr/bin/env python3
"""Compare airborne animation GET 30 with the local retail executable."""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd();unit,mover,kind=[HEAP+i*0x10000 for i in range(3)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(unit+8,mover);put(unit+0xb4,kind);put(kind+0x260,0x800)
rng=random.Random(0x4dc1f0);rows=[];expected=[]
for i in range(4096):
    velocity=rng.randrange(-20*65536,20*65536)
    speed=rng.randrange(1,16*65536)
    blocked=int(i%11==0);attached=int(i%13==0)
    put(unit+0xa8,attached);put(unit+0x12b,speed);put(mover+12,velocity)
    p.uc.mem_write(mover+0x36,struct.pack('<H',4 if blocked else 0))
    native,error=p.call(0x4dc1f0,(unit,),ecx=mover)
    if error:raise RuntimeError(error)
    expected.append(native if native<0x80000000 else native-0x100000000)
    rows.append(f'{velocity} {speed} {blocked} {attached}')
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--vertical-animation'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=list(map(int,result.stdout.split()))
assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 4096 airborne GET 30 queries match retail, including attachment and refusal')
