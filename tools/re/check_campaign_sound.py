#!/usr/bin/env python3
"""Native mission sound host 4d3580: global routing, priority/free voice, zero return."""
import struct
import subprocess
import sys
from emu import Icd, HEAP
p=Icd()
game=HEAP;name=HEAP+0x10000
p.uc.mem_write(0x62d55c,struct.pack('<I',game))
free=False;played=[]
p.hooks[0x56eb60]=lambda uc,sp:(0,int(free))
def play(uc,sp):
    played.append(struct.unpack('<4I',uc.mem_read(sp,16)))
    return 4,123
p.hooks[0x50a720]=play
p.freeze_hooks()
rows=[];expected=[]
for flags in range(256):
    for free in (False,True):
        played.clear()
        result,error=p.call(0x4d3580,(name,flags),ecx=HEAP+0x20000)
        assert error is None and result==0,(error,result)
        priority=flags&7
        assert played==([(name,priority,0,127)] if priority>1 or free else []),played
        rows.append(f'{flags} {int(free)}');expected.append(priority if played else -1)
actual=subprocess.check_output([sys.argv[1] if len(sys.argv)>1 else 'build-o2/campaign_audio_test','--routes'],
                              input='\n'.join(rows)+'\n',text=True)
assert list(map(int,actual.split()))==expected
print('PASS: 512 native mission sound routes; global nonlooping priority, free-voice gate, zero return')

# Objective completion audio uses a second flag, separate from completion.
# DestroyAllUnits may go false again; the cue must not replay when it returns.
p=Icd();game=HEAP;condition=HEAP+0x30000;unit=HEAP+0x40000
owner=HEAP+0x50000;side=HEAP+0x60000;definition=HEAP+0x70000
put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
played=[]
def objective_sound(uc,sp):
    pointer,priority,loop,volume=struct.unpack('<4I',uc.mem_read(sp,16))
    name=bytes(uc.mem_read(pointer,32)).split(b'\0')[0]
    played.append((name,priority,loop,volume));return 4,0
p.hooks[0x50a720]=objective_sound
p.hooks[0x5d5bb0]=lambda uc,sp:(0,0)
p.freeze_hooks();put(0x62d55c,game)
expected=[(b'Victory Condition',7,0,127)]
for enemy_count in (1,0,1,0,0):
    p.uc.mem_write(game+0x25fc,struct.pack('<H',enemy_count))
    result,error=p.call(0x523770,(),ecx=condition)
    assert error is None and result==int(enemy_count==0),(result,error)
assert played==expected and get(condition+8)==1,played
played.clear();p.uc.mem_write(condition,bytes(128))
put(unit+0xb8,owner);put(owner+0x50,side);put(unit+0xb4,definition)
p.uc.mem_write(unit+0xfd,b'\1')
for _ in range(3):
    result,error=p.call(0x523670,(unit,),ecx=condition)
    assert error is None,error
assert played==expected and get(condition+4)==1 and get(condition+8)==1,played
played.clear();put(condition+0xc,30)
for tick in (0,29,30,31):
    put(game+0x19f44,tick)
    result,error=p.call(0x524270,(),ecx=condition)
    assert error is None and result==int(tick>=30),(result,error)
assert not played,played
print('PASS: native objective cue name/priority, independent once-only poll/event latch; silent timer')
