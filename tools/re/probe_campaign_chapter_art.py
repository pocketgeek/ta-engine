#!/usr/bin/env python3
"""Native4a9640 chapter illustration indexing, without opening the retail game.

Campaign/GUI accessors and string comparison are controlled services; native
campaign-name branches and frame bounds execute. Stop before title translation.
"""
import struct
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
p=Icd()
camp,mission,gadget,screen,text=[HEAP+i*0x1000 for i in range(5)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def string(a):
 b=bytearray()
 while True:
  c=p.uc.mem_read(a+len(b),1)[0]
  if not c:return bytes(b)
  b.append(c)
selected=[]
def find(uc,sp):return 2,gadget if string(read(sp))==b'ChapterImage' else 0
def compare(uc,sp):return 0,int(string(read(sp)).lower()!=string(read(sp+4)).lower())
def setframe(uc,sp):selected.append(read(sp));return 1,0
p.hooks.update({0x4a6680:lambda u,s:(0,camp),0x4a5da0:lambda u,s:(0,mission),
 0x572840:find,0x5d5bb0:compare,0x572e80:setframe})
p.freeze_hooks()
p.uc.hook_add(UC_HOOK_CODE,lambda u,a,n,d:u.emu_stop(),begin=0x4a9740,end=0x4a9740)
# Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
p.allow_early_stop=True
cases=0
for name in ('book of darien.tdf','the iron plague.tdf','IPalt.tdf','custom.tdf'):
 for chapter in (0,1,23,24,47,48,90):
  for count in (1,15,75,100):
   p.uc.mem_write(text,name.encode()+b'\0');put(camp+4,text);put(mission+0x10,chapter);put(gadget+0x40,count)
   selected.clear();_,error=p.call(0x4a9640,(),ecx=screen)
   assert not error,error
   wanted=chapter+1 if name=='book of darien.tdf' else chapter+50 if name in ('the iron plague.tdf','IPalt.tdf') else 49
   wanted=wanted if wanted<count else 0
   assert selected==[wanted],(name,chapter,count,selected,wanted)
   cases+=1
print(f'PASS: {cases} native BOD chapter image selections, campaign offsets and out-of-range frame zero')
