#!/usr/bin/env python3
"""Run official 3.0 damage blocks without launching retail (pefile + Unicorn).

Supply the independently fingerprinted KINGDOMS.icd; no retail bytes are included.
"""
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable",type=str)
args=parser.parse_args()
import hashlib,struct
from pathlib import Path
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn.x86_const import *
b=Path(args.executable).read_bytes()
assert hashlib.sha256(b).hexdigest()=='6ddc7fae0cbf3ee13d530614b64608a3aeefca7deb73719310a4f68fe1f22e96'
u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x400000,0x400000);u.mem_write(0x400000,pefile.PE(data=b).get_memory_mapped_image());u.mem_map(0x10000000,0x40000)
p=lambda n:struct.pack('<I',n&0xffffffff)
w=0x10000000;node=0x10001000;frame=0x10020000;stack=0x10030000;stop=0x1003f000
# Execute the native CRT precision initializer, including _controlfp and fldcw.
u.mem_write(stack,p(stop));u.reg_write(UC_X86_REG_ESP,stack)
u.reg_write(UC_X86_REG_FPCW,0x37f)
u.emu_start(0x5dac26,stop,count=1000)
print("Initializer CW",hex(u.reg_read(UC_X86_REG_FPCW))); assert (u.reg_read(UC_X86_REG_FPCW)&0x300)==0x200
for cw,expected_boundary in [(0x27f,3),(0x37f,2)]:
 for base,mult,expected in [(10,.3,expected_boundary),(301,.5,150),(100,.29,28),
                            (65535,1.1,72088),(91,-.5,-45),(360,2,720),(180,.04,7)]:
  u.mem_write(w+0x88,struct.pack('<H',base))
  u.mem_write(frame-0x38,struct.pack('<d',mult))
  for reg,val in [(UC_X86_REG_EBX,w),(UC_X86_REG_ESI,node),
                  (UC_X86_REG_EBP,frame),(UC_X86_REG_ESP,stack),
                  (UC_X86_REG_FPCW,cw)]:u.reg_write(reg,val)
  u.emu_start(0x531b4b,0x531b68,count=100)
  actual=struct.unpack('<i',u.mem_read(node+0x10,4))[0]
  assert actual==expected,(cw,base,mult,expected,actual)
print('Native CRT precision and 14 damage arithmetic cases passed')
# Lookup: a one-node map, using native search and lookup (no hooks).
m=0x10002000;header=0x10003000;nil=0x10004000;t=0x10005000;d=0x10006000
u.mem_write(w+0x44,p(m));u.mem_write(w+0x88,struct.pack('<H',301));u.mem_write(m+4,p(header)+p(nil));u.mem_write(header+4,p(node));u.mem_write(node,p(nil)+p(0)+p(nil)+p(77)+p(150));u.mem_write(t+0xb4,p(d))
for key,expected in [(77,150),(76,301),(78,301),(0,301)]:
 u.mem_write(d+0x9e,p(key));u.mem_write(stack,p(stop)+p(t));u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_ECX,w);u.emu_start(0x531e50,stop,count=500)
 assert u.reg_read(UC_X86_REG_EAX)==expected
print('4 native target-category lookup cases passed')
