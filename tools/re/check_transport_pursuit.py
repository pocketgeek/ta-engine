#!/usr/bin/env python3
"""Compare native air pickup pursuit geometry with the port.

Runs the pursuit constructor, radius setter, target getter and arrival predicate.
Only reference bookkeeping is substituted; the target-coordinate routines run
in the user-owned executable. Does not replace a complete mission/mover trace.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW
p=Icd()
carrier,passenger,kind,mission,game,controller,point=[HEAP+i*0x1000 for i in range(7)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def init(uc,sp):
    address=uc.reg_read(UC_X86_REG_ECX);uc.mem_write(address,bytes(16));return 2,address

def bind(uc,sp):
    address=uc.reg_read(UC_X86_REG_ECX);put(address+4,read(sp));return 1,address
p.hooks.update({0x519990:init,0x5199f0:bind});p.freeze_hooks()
p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
put(0x62d55c,game);put(game+0x19f30,1)
put(passenger+0xb4,kind);put(mission+0xe,carrier)
rng=random.Random(0x4e3f70);rows=[];expected=[]
for case in range(4096):
    target=[rng.randrange(-2000*65536,2000*65536),rng.randrange(-100*65536,800*65536),rng.randrange(-2000*65536,2000*65536)]
    radius=rng.randrange(16,1000)
    position=[target[0]+rng.randrange(-1500*65536,1500*65536),rng.randrange(500*65536),target[2]+rng.randrange(-1500*65536,1500*65536)]
    if case%4==0:position[0]=target[0]+(radius-1)*65536+rng.choice((-1,0,1));position[2]=target[2]
    p.uc.mem_write(carrier+0x68,struct.pack('<3i',*position))
    p.uc.mem_write(passenger+0x68,struct.pack('<3i',*target))
    for address,args in ((0x4e3f70,(mission,passenger)),(0x4e4540,(radius-1,)),(0x4e41d0,(point,))):
        result,error=p.call(address,args,ecx=controller)
        assert not error,error
    got=struct.unpack('<3i',p.uc.mem_read(point,12))
    accepted,error=p.call(0x4e43d0,(carrier,),ecx=controller)
    assert not error,error
    flags,reach=struct.unpack('<Hh',p.uc.mem_read(controller+8,4))
    expected.append((*got,flags,reach,accepted))
    rows.append(' '.join(map(str,position+target+[radius])))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-pursuit'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 4096 native air pickup pursuit targets, height clamps, radii and arrival predicates')
