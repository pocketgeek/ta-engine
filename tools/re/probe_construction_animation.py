#!/usr/bin/env python3
"""Execute construction particle startup/update with native sprite clocks.

No routine replacements. Synthetic authored durations isolate clock behavior.
"""
import struct
from emu import Icd,HEAP
p=Icd();particle,owner,position,animation=[HEAP+i*0x10000 for i in range(4)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
p.freeze_hooks();checks=0
for durations in ([2]*12,[1,3,5],[0,2,1]):
 for looping in [0,1]:
  data=bytearray(0x30+len(durations)*8)
  struct.pack_into('<HB',data,0,len(durations),looping)
  for i,d in enumerate(durations):struct.pack_into('<H',data,0x2c+i*8,d)
  p.uc.mem_write(animation,bytes(data))
  for rising in [0,1]:
   put(position,0,0,0);put(owner+0x6c,0)
   _,error=p.call(0x4f1310,(owner,position,65536,100*65536,animation,rising),ecx=particle)
   assert not error,error
   frame=0;remaining=durations[0];active=True
   def check():
    global checks
    f,r,loop=struct.unpack('<HHB',p.uc.mem_read(particle+0x18,5))
    ptr=struct.unpack('<I',p.uc.mem_read(particle+0x20,4))[0]
    assert (f,r,loop,bool(ptr))==(frame,remaining,looping,active)
    checks+=1
   check()
   for tick in range(64):
    result,error=p.call(0x4f12d0,(),ecx=particle)
    assert not error and result&255==1,(error,result)
    if active:
     if remaining>=2:remaining-=1
     else:
      frame+=1
      if frame==len(durations):
       if looping:frame=0
       else:active=False
      if active:remaining=durations[frame]
    check()
print(f'PASS: {checks} native construction animation snapshots; frame zero startup, authored delays, per-particle single-tick clock')
