#!/usr/bin/env python3
"""Native missing-callback lookup and weapon handshake.

The native script name lookup/start paths run with an empty or unrelated script
roster. Geometry, range predicates, display transport and CRT are controlled.
No script acknowledgement is synthesized by a missing AimWeapon/FireWeapon.
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd()
unit,kind,weapon,script,definition,names,strings,vtable=[HEAP+i*0x10000 for i in range(8)]
record=unit+12

def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def short(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
def word(a):return struct.unpack('<H',p.uc.mem_read(a,2))[0]
def geometry(uc,sp):
    uc.mem_write(record+0x16,struct.pack('<2H',1234,5678));return 2,0

def point(uc,sp):
    out=struct.unpack('<I',uc.mem_read(sp+4,4))[0]
    uc.mem_write(out,bytes(12));return 3,1

p.hooks.update({0x52fd80:geometry,0x51a9a0:lambda uc,sp:(2,0),0x51aa50:point,
    0x530630:lambda uc,sp:(3,0),HEAP+0x80000:lambda uc,sp:(3,1),
    0x4ea4f0:lambda uc,sp:(4,0),0x4ea560:lambda uc,sp:(2,0),
    0x5d4444:lambda uc,sp:(0,16384)})
p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
put(unit+0xb4,kind);put(unit+0xbc,script);put(script+0xc,definition)
put(record,weapon);put(weapon,vtable);put(vtable+0x10,HEAP+0x80000)
put(definition+0x1c,names);put(names,strings);p.uc.mem_write(strings,b'Create\0')
short(weapon+0x9c,30)
p.uc.mem_write(unit+0xf4,struct.pack('<f',1.0))
p.uc.mem_write(unit+0xd8,struct.pack('<f',100.0))
count=0
for script_count in (0,1):
    put(definition+4,script_count)
    for slot in range(3):
        for extra in range(256):
            flags=(extra<<8)|slot
            short(record+0x1a,flags);short(record+0x14,0)
            result,error=p.call(0x52fe30,(unit,record),ecx=weapon);assert not error,error
            assert result==1 and word(record+0x1a)==flags|0xe0
            ready,error=p.call(0x52fff0,(unit,record),ecx=weapon);assert not error,error
            assert ready==0 and word(record+0x1a)==flags|0xe0
            # Even with aim explicitly acknowledged, absent FireWeapon does
            # not acknowledge SET 23. Reload starts, but no shot is requested.
            short(record+0x1a,flags|8)
            _,error=p.call(0x530140,(unit,record));assert not error,error
            assert word(record+0x1a)==flags|8 and word(record+0x14)==30
            count+=1
print(f'PASS: {count} native missing-callback pairs; real lookup leaves aim unacknowledged and does not synthesize a fire acknowledgement')
