#!/usr/bin/env python3
"""Observe retail CRT placement/name fields; reads the user's retail binary only."""
import struct
from emu import Icd, HEAP, STACK
from unicorn import UC_HOOK_MEM_READ
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_EAX, UC_X86_REG_ESP

p=Icd()
def put(a,fmt,*v): p.uc.mem_write(a,struct.pack(fmt,*v))
frame=STACK+0x10000
kind,pos,unit,text,out=HEAP,HEAP+0x1000,HEAP+0x2000,HEAP+0x3000,HEAP+0x4000
p.hooks[0x507990]=lambda uc,args:(2,37) # native terrain sampler boundary
reads=[]
p.uc.hook_add(UC_HOOK_MEM_READ,lambda uc,access,a,size,value,data:reads.append(a),begin=frame-0x1d0,end=frame-0x1c5)
for mobile in (0,1):
 for width,depth in ((1,1),(2,3),(5,4)):
  for y in (-100,0,200,999):
   put(kind+0x126,'<hh',width,depth);put(kind+0x24a,'<B',mobile)
   put(frame-0x1d0,'<iii',12,y,19)
   put(frame-0x60,'<i',123456) # poison intermediate Y: raw CRT Y must not replace it
   p.uc.reg_write(UC_X86_REG_EBP,frame);p.uc.reg_write(UC_X86_REG_EAX,kind)
   p.uc.reg_write(UC_X86_REG_ESP,STACK+0x80000)
   p.uc.emu_start(0x4cc8e6,0x4cc920)
   xyz=struct.unpack('<iii',p.uc.mem_read(frame-0x64,12))
   assert xyz==((12*16+width*8)<<16,123456 if mobile else 37<<16,(19*16+depth*8)<<16),xyz
assert frame-0x1cc not in reads,'retail read CRT vertical field'
# Execute real rename, replacing only allocator/free and byte-copy boundaries.
p.hooks[0x5ba3d0]=lambda uc,args:(0,out)
p.hooks[0x5ba5d0]=lambda uc,args:(0,0)
def copy(uc,args):
 dst,src,n=struct.unpack('<III',uc.mem_read(args,12));uc.mem_write(dst,bytes(uc.mem_read(src,n)));return 0,dst
p.hooks[0x5d4570]=copy
for name in (b'',b'My monarch',b'a'*31,b'b'*32,b'c'*255):
 put(unit+4,'<I',0);p.uc.mem_write(text,name+b'\0')
 _,error=p.call(0x51f4c0,(unit,text,0));assert not error,error
 actual=bytes(p.uc.mem_read(out,min(32,len(name)+1))).split(b'\0')[0]
 assert actual==name[:31],actual
print('PASS: 24 native placement conversions (CRT Y unread), 5 native name cases')
