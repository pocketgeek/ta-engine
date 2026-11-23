#!/usr/bin/env python3
"""Execute native flight arrival notification and controller retention.

Reference bookkeeping and event delivery are sinks; native point/pursuit
construction, goal queries, flight navigation update and detachment execute.
"""
import struct
import subprocess
import sys
from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p=Icd()
nav,unit,kind,controller,mission,point,target=[HEAP+i*0x1000 for i in range(7)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
events=[]
def init(uc,sp):
 a=uc.reg_read(UC_X86_REG_ECX);uc.mem_write(a,bytes(16));return 2,a
def bind(uc,sp):
 a=uc.reg_read(UC_X86_REG_ECX);put(a+4,get(sp));return 1,a
def notify(uc,sp):events.append(get(sp));return 1,0
p.hooks.update({0x519990:init,0x5199f0:bind,0x4e2470:notify,
                0x4d91b0:lambda uc,sp:(3,0),0x4da620:lambda uc,sp:(2,0)})
p.freeze_hooks()
put(0x62d55c,HEAP+0x10000);put(HEAP+0x10000+0x19f30,1)
put(nav,0x5f34d4);put(nav+8,unit);put(unit+0xb4,kind);put(mission+0xe,unit)
put(unit+0x68,400*65536,100*65536,400*65536);put(target+0x68,400*65536,100*65536,400*65536);put(target+0xb4,kind)
put(unit+0xa4,HEAP+0x9000);p.uc.mem_write(HEAP+0x9001,b'\x64')
put(point,400*65536,100*65536,400*65536)
put(nav+12,400*65536,100*65536,400*65536)
for pursuit in (False,True):
 result,error=p.call(0x4e3f70 if pursuit else 0x4e40e0,(mission,target if pursuit else point),ecx=controller)
 assert not error,error
 _,error=p.call(0x4e4540,(116,),ecx=controller);assert not error,error
 put(nav+4,controller);events.clear()
 _,error=p.call(0x524af0,ecx=nav)
 assert not error,error
 assert events==([0x100] if pursuit else [0x100,0x400]),(pursuit,events)
 assert get(nav+4)==(controller if pursuit else 0)
 print(f'PASS: native {"pursuit" if pursuit else "point"} arrival events={events}, controller retained={bool(get(nav+4))}')
 if not pursuit:
  retained=bytes(p.uc.mem_read(nav+12,26));events.clear()
  for tick in range(32):
   put(unit+0x68,(400+tick)*65536)
   _,error=p.call(0x524af0,ecx=nav)
   assert not error,error
   assert bytes(p.uc.mem_read(nav+12,26))==retained and not events
  out=HEAP+0xa000
  _,error=p.call(0x524ab0,(out,out+12,out+24),ecx=nav)
  assert not error,error
  assert bytes(p.uc.mem_read(out,26))==retained
  # The flight kernel still uses the retained navigator outputs with no
  # controller installed. Orientation/banking are isolated boundary sinks.
  mover=HEAP+0xb000
  put(unit+8,mover);put(mover,nav);put(mover+8,65536,0,0);put(mover+0x20,65536)
  put(unit+0x12b,131072);put(kind+0x166,4096,16384)
  p.uc.mem_write(mover+0x36,struct.pack('<H',2))
  _,error=p.call(0x4da7d0,(unit,),ecx=mover)
  assert not error,error
  velocity=struct.unpack('<3i',p.uc.mem_read(mover+8,12))
  assert velocity!=(0,0,0) and velocity!=(65536,0,0),velocity
  print('PASS: detached flight navigator retains target/drift/heading across 32 updates; native flight velocity continues updating',velocity)
  put(unit+0x68,400*65536)

# Exercise the whole native arrival/retention path one fixed-point quantum
# inside, exactly on, and outside each horizontal boundary. Height does not
# constrain these radius-based controllers.
rows=[];expected=[]
for pursuit in (False,True):
 for radius in (16,116,149,500):
  for axis in (0,2):
   for sign in (-1,1):
    for delta in (-1,0,1):
     for height in (-100,100,700):
      position=[400*65536,height*65536,400*65536]
      position[axis]+=sign*(radius*65536+delta)
      put(unit+0x68,*position);put(nav+12,400*65536,100*65536,400*65536)
      _,error=p.call(0x4e3f70 if pursuit else 0x4e40e0,
                     (mission,target if pursuit else point),ecx=controller)
      assert not error,error
      _,error=p.call(0x4e4540,(radius,),ecx=controller);assert not error,error
      put(nav+4,controller);events.clear()
      _,error=p.call(0x524af0,ecx=nav);assert not error,error
      arrived=delta<0
      expected_events=([0x100] if pursuit else [0x100,0x400]) if arrived else []
      assert events==expected_events,(pursuit,radius,position,events)
      assert get(nav+4)==(0 if arrived and not pursuit else controller)
      rows.append(' '.join(map(str,position+[400*65536,100*65536,400*65536,radius+1])))
      expected.append(int(arrived))
# Both constructor variants set the radius flag; the compiled pursuit CLI
# exercises the shared RetailFlightGoal::accepts predicate for these inputs.
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--pickup-pursuit'],
 input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[int(line.split()[-1]) for line in result.stdout.splitlines()]
assert actual==expected
print(f'PASS: {len(rows)} native point/pursuit arrival boundaries, altitude independence and retention decisions agree with the compiled radius predicate')
