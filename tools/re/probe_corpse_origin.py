#!/usr/bin/env python3
"""Native corpse placement keeps model position/angles separate from map cells."""
import struct
from emu import HEAP,Icd
from unicorn.x86_const import UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI
p=Icd();uc=p.uc
unit,definition,game,cell,feature,frame=[HEAP+i*0x10000 for i in range(1,7)]
def put(a,v):uc.mem_write(a,struct.pack('<I',v&0xffffffff))
put(0x62d55c,game);put(unit+0xb4,definition)
uc.mem_write(unit+0x74,struct.pack('<2h',30,26))
uc.mem_write(definition+0x25c,struct.pack('<2h',0,2))
uc.mem_write(definition+0x24e,struct.pack('<H',0))
position=(512*65536,100*65536,512*65536);angles=(0x2000,17,29)
uc.mem_write(unit+0x68,struct.pack('<3i',*position));uc.mem_write(unit+0x7c,struct.pack('<3H',*angles))
anchors=[];requests=[]
def lookup(_uc,sp):
 anchors.append(struct.unpack('<2i',uc.mem_read(sp,8)));return 2,cell
def install(_uc,sp):
 args=struct.unpack('<5I',uc.mem_read(sp,20));requests.append(args);return 5,0
p.hooks[0x50e600]=lookup;p.hooks[0x511170]=lambda _uc,_sp:(1,100)
p.hooks[0x495360]=install;p.freeze_hooks()
_,error=p.call(0x512ee0,(unit,1,0,0,0));assert error is None,error
assert anchors==[(30,28)],anchors
assert len(requests)==1 and requests[0][2:4]==(unit+0x68,unit+0x7c),requests
# Execute the native feature object's position/orientation copy branch itself.
put(frame+0x10,requests[0][2]);put(frame+0x14,requests[0][3])
uc.reg_write(UC_X86_REG_EBP,frame);uc.reg_write(UC_X86_REG_ESI,feature);uc.reg_write(UC_X86_REG_EDI,0)
uc.emu_start(0x4954fb,0x495562,count=100)
assert tuple(struct.unpack('<3i',uc.mem_read(feature+8,12)))==position
assert tuple(struct.unpack('<3H',uc.mem_read(feature+0x20,6)))==angles
print('PASS: native corpseadjust moves map anchor to (30,28); model retains original position and all three angles')
