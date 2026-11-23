#!/usr/bin/env python3
"""Execute native mission controller replacement and event-bit clearing.

Only navigator virtual installation and controller destruction are sinks.
This checks ownership/order at the mission boundary, not navigator movement.
"""
import struct
import random
from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p=Icd()
mission,unit,mover,navigator,navtable,old,oldtable,new,api=[HEAP+i*0x1000 for i in range(9)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
calls=[]
def install(uc,sp):
 assert uc.reg_read(UC_X86_REG_ECX)==navigator
 calls.append(('install',read(sp)));return 1,0
def destroy(uc,sp):
 assert uc.reg_read(UC_X86_REG_ECX)==old and read(sp)==1
 calls.append(('destroy',old));return 1,0
p.hooks.update({api:install,api+16:destroy});p.freeze_hooks()
put(mission+0xe,unit);put(mover,navigator);put(navigator,navtable);put(navtable+4,api);put(old,oldtable);put(oldtable,api+16)
rng=random.Random(0x4d4d40)
count=0
for has_mover in (False,True):
 for old_present in (False,True):
  for new_present in (False,True):
   for _ in range(256):
    flags=rng.getrandbits(32);put(mission+0x6a,flags);put(mission+0x6e,old if old_present else 0)
    put(unit+8,mover if has_mover else 0);calls.clear()
    result,error=p.call(0x4d4d40,(new if new_present else 0,),ecx=mission)
    assert not error,error
    expected=[]
    if has_mover:
     if old_present:expected=[('install',0),('destroy',old)]
     expected.append(('install',new if new_present else 0))
    assert calls==expected,(calls,expected)
    expected_controller=(new if new_present else 0) if has_mover else (old if old_present else 0)
    assert read(mission+0x6e)==expected_controller
    assert read(mission+0x6a)==(flags&~0x3700 if has_mover and new_present else flags)
    count+=1
print(f'PASS: {count} native controller replacements preserve detach/destroy/install ordering and clear only the expected pending movement events')
