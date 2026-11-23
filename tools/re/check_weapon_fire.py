#!/usr/bin/env python3
"""Compare native FireWeapon reload/event/callback sequencing with shared code.

530140 executes fully; the CRT roll and script/display callback sinks are
controlled. This does not establish global CRT-stream ordering in a match.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
p=Icd()
unit,kind,record,weapon=[HEAP+i*0x10000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def short(a,v):p.uc.mem_write(a,struct.pack('<H',v&65535))
def word(a):return struct.unpack('<H',p.uc.mem_read(a,2))[0]
roll=0;trace=[]
def random_sink(uc,sp):trace.append(('random',));return 0,roll

def callback(uc,sp):
    args=struct.unpack('<8I',uc.mem_read(sp,32))
    name=bytes(uc.mem_read(args[0],32)).split(b'\0')[0]
    assert name==b'FireWeapon' and args[3]==1
    trace.append(('script',args[4],word(record+0x14),read(unit+0xd0)))
    return 8,0

def display(uc,sp):
    assert read(sp)==unit
    trace.append(('display',read(sp+4)&255));return 2,0

def unexpected_sound(uc,sp):
    raise AssertionError('native FireWeapon invented audio outside its script callback')

# Both named unit routes and resolved positional/global sound submissions.
# FireWeapon itself must not supply a guessed weapon-kind sound when its
# authored callback is silent (the controlled callback above emits nothing).
p.hooks.update({0x5d4444:random_sink,0x56c640:callback,0x4ea560:display,
                0x50a9c0:unexpected_sound,0x50a7d0:unexpected_sound,
                0x50a720:unexpected_sound,0x50a6b0:unexpected_sound})
p.freeze_hooks();put(unit+0xb4,kind);put(record,weapon)
rng=random.Random(0x530140);rows=[];expected=[]
cases=[(n,r) for n in (0,1,4,5,6,30,32767,54613,65535) for r in (0,1,16384,32767)]
cases += [(rng.randrange(65536),rng.randrange(32768)) for _ in range(4096)]
for nominal,roll in cases:
    wf=rng.getrandbits(24);tf=rng.getrandbits(24);uf=rng.getrandbits(32);af=rng.randrange(65536)
    short(weapon+0x9c,nominal);put(weapon+0xc8,wf);put(kind+0x260,tf);put(unit+0x130,uf)
    short(record+0x1a,af);short(record+0x14,123);put(unit+0xd0,0);trace.clear()
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    _,error=p.call(0x530140,(unit,record));assert not error,error
    reload,event=word(record+0x14),read(unit+0xd0)
    assert trace==[('random',),('script',af&3,reload,event),('display',af&3)],trace
    assert word(record+0x1a)==af
    rows.append(f'{nominal} {roll} {wf} {tf} {uf} {af}');expected.append((reload,event))
result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--weapon-fire'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(expected),(len(actual),len(expected))
for i,(a,e) in enumerate(zip(actual,expected)):
    assert a==e,(rows[i],a,e)
print(f'PASS: {len(rows)} native FireWeapon reloads/events, one CRT draw even for zero spread, script/display callback order and slot identity; no fabricated firing audio')

# The display receiver likewise just invokes FireWeapon. Exercise it with a
# silent callback so guessed audio in either half of dispatch is detectable.
game=HEAP+0x80000
units=HEAP+0xa0000
display_unit=units+0x138
packet=HEAP+0xc0000
put(0x62d55c,game)
put(game+0x14e84,units);put(game+0x14e88,units+0x138*2)
put(display_unit+0x130,0x1000000)
put(display_unit+0xbc,HEAP+0xd0000)
for slot in range(256):
    p.uc.mem_write(packet,bytes([0x10,1,0,slot]))
    trace.clear()
    _,error=p.call(0x4ea640,(packet,));assert not error,error
    assert len(trace)==1 and trace[0][0:2]==('script',slot),trace
print('PASS: 256 native display FireWeapon callbacks, no fabricated firing audio')
