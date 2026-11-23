#!/usr/bin/env python3
"""Check native circle/ring/rectangle arrival detachment.

Only notification, scheduler cancellation and controller deallocation are host
boundaries. Satisfaction queries and navigator detachment execute original code.
The matching World regression is completedPointDetaches in retail_trace_test.cpp.
"""
import struct
from emu import Icd,HEAP
p=Icd();u=p.uc
nav,unit,mover,controller,mission,player,game,worker=[HEAP+n*0x1000 for n in range(8)]
def put(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def get(a):return struct.unpack('<I',u.mem_read(a,4))[0]
put(0x62d55c,game);put(game+0x19f44,200);put(game+0x19e70,worker)
put(nav,0x5f2a24);put(nav+8,unit);put(unit+8,mover);put(mover,nav);put(unit+0xb8,player)
u.mem_write(unit+0x74,struct.pack('<4h',8,8,1,1));put(unit+0x68,128*65536);put(unit+0x70,128*65536)
put(controller+4,mission);put(mission+0xe,unit)
events=[]
p.hooks[0x415f30]=lambda uc,a:(1,0)
p.hooks[0x4eba00]=lambda uc,a:(0,0)
def notify(uc,a):events.append(get(a));return 1,0
p.hooks[0x4e2470]=notify
p.freeze_hooks()
cases = [
 ('circle',0x5f28d8,struct.pack('<hhii',8,8,4,0)),
 ('ring',0x5f290c,struct.pack('<hhiii',8,8,0,4,0)),
 ('rectangle',0x5f2940,struct.pack('<4i',8,10,8,10)),
]
for name,table,fields in cases:
 for stamp in (100,199):
  put(controller,table);u.mem_write(controller+8,fields)
  put(nav+4,controller);put(mission+0x6e,controller)
  put(nav+0x10c,2);put(nav+0x110,stamp);u.mem_write(nav+0x114,b'\x01')
  u.mem_write(nav+12,struct.pack('<4h',128,128,136,128));events.clear()
  result,error=p.call(0x4e5150,ecx=nav)
  observed=(get(nav+4),bytes(u.mem_read(nav+0x114,1)),get(nav+0x10c),get(nav+0x110),list(events))
  expected=(0,b'\x08',2,stamp if stamp==199 else 0,[256,1024])
  assert error is None and observed==expected,(name,stamp,error,observed,expected)
  print(f'PASS {name} arrival stamp={stamp}: controller detached, stored points retained, events=0x500')
