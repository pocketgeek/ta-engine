#!/usr/bin/env python3
"""Compare native per-storm sampler state and double return against shared C++."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESI

p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
entry,state,output=HEAP,HEAP+0x1000,HEAP+0x2000
# Synthetic caller stores the native x87 return as a double for comparison.
code=b'\x55\x89\xe5\xff\x75\x0c\xff\x75\x08\xe8'
code+=struct.pack('<i',0x52f780-(entry+14))
code+=b'\xdd\x1d'+struct.pack('<I',output)+b'\x89\xec\x5d\xc2\x08\x00'
p.uc.mem_write(entry,code)
p.hooks[0x52faa0]=lambda uc,sp:(0,0)
p.freeze_hooks()
rng=random.Random(0x52f780);rows=[];expected=[]
for case in range(8192):
    seed=[0,1,0x7fffffff,0x80000000,0xffffffff][case] if case<5 else rng.getrandbits(32)
    span=struct.unpack('<f',struct.pack('<f',rng.uniform(-1000,1000)))[0] if case%8 else 0.0
    bits=struct.unpack('<I',struct.pack('<f',span))[0]
    p.uc.mem_write(state,struct.pack('<I',seed))
    _,error=p.call(entry,(state,bits if bits<0x80000000 else bits-0x100000000))
    assert not error,(case,error)
    actual=struct.unpack('<I',p.uc.mem_read(state,4))[0]
    next_state=(seed*16807-(seed//127773)*0x7fffffff)&0xffffffff
    if not next_state:next_state=0x7fffffff
    result=struct.unpack('<Q',p.uc.mem_read(output,8))[0]
    predicted=struct.unpack('<Q',struct.pack('<d',float(next_state)*2**-31*span))[0]
    assert (actual,result)==(next_state,predicted),(case,actual,next_state,result,predicted)
    rows.append(f'{seed} {bits}');expected.append((actual,result))
    shot,aim=HEAP+0x3000,HEAP+0x4000
    heading,pitch=rng.randrange(65536),rng.randrange(65536)
    p.uc.mem_write(aim+0x16,struct.pack('<HH',heading,pitch))
    p.uc.reg_write(UC_X86_REG_ESI,shot)
    _,error=p.call(0x52fa7b,(),ecx=aim)
    assert not error,(case,error)
    assert struct.unpack('<I',p.uc.mem_read(shot+0xb8,4))[0]==heading|(pitch<<16)

if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--wander-random'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    assert [tuple(map(int,line.split())) for line in result.stdout.splitlines()]==expected
print('PASS: 8192 native per-storm RNG states and double samples, including zero span and unsigned seed boundaries')
