#!/usr/bin/env python3
"""Compare moving-target aim points with native 51aa50 and controlled SweetSpot."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd();game,pool,kind,weapon,mover,out=[HEAP+i*0x20000 for i in range(6)]
shooter=pool+312;target=pool+624
point=(0,0,0)
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def sweet(uc,sp):
    dest=struct.unpack('<I',uc.mem_read(sp+4,4))[0]
    uc.mem_write(dest,struct.pack('<3i',*point));return 2,0
p.hooks[0x4dd4f0]=sweet;p.freeze_hooks()
put(0x62d55c,game);put(game+0x14e84,pool);put(game+0x14e88,target)
put(shooter+0xb4,kind);put(shooter+0xc,weapon)
p.uc.mem_write(shooter+0x10,struct.pack('<2H',2,0x8000))
put(target+0x130,0x1000000);put(target+8,mover)
rng=random.Random(0x51aa50);rows=[];expected=[]
for i in range(8192):
    source=[rng.randrange(0,2000*65536) for _ in range(3)]
    point=tuple(rng.randrange(0,2000*65536) for _ in range(3))
    velocity=[rng.randrange(-10*65536,10*65536) for _ in range(3)]
    speed=0 if i%31==0 else rng.randrange(65536,60*65536)
    noLead=int(i%13==0)
    p.uc.mem_write(shooter+0x68,struct.pack('<3i',*source))
    p.uc.mem_write(mover+8,struct.pack('<3i',*velocity))
    put(weapon+0xcc,speed);put(weapon+0xd0,1);put(weapon+0xc8,0x80000 if noLead else 0)
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    result,error=p.call(0x51aa50,(shooter,out,0))
    if error or result!=1:raise RuntimeError((result,error))
    expected.append(struct.unpack('<3i',p.uc.mem_read(out,12)))
    rows.append(' '.join(map(str,[*source,*point,*velocity,speed,noLead])))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--aim-lead'],
                      input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 8192 native moving-target aim points, including vertical motion and no-lead/zero-speed cases')
