#!/usr/bin/env python3
"""Observe common native weapon update sequencing, with controlled aim/fire sinks.

Runs 52ae90's slot selection, reload countdown, ready/fire split and SET-23
projectile admission. Does not emulate script execution or individual weapons.
"""
import random
import struct
import subprocess
import sys
from emu import Icd,HEAP
p=Icd()
unit,kind,game=[HEAP+i*0x10000 for i in range(3)]
weapons=[HEAP+(3+i)*0x10000 for i in range(3)]
trace=[];starts=[];ready=[];signal=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def short(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
def word(a):return struct.unpack('<H',p.uc.mem_read(a,2))[0]
def slot(sp):
    assert read(sp)==unit
    return (read(sp+4)-unit-12)//28

def start(uc,sp):
    i=slot(sp);trace.append(('aim',i));return 2,starts[i]
def gate(uc,sp):
    i=slot(sp);trace.append(('ready',i));return 2,ready[i]
def fire(uc,sp):
    i=slot(sp);trace.append(('fire_callback',i))
    if signal[i]:short(unit+12+i*28+0x1a,word(unit+12+i*28+0x1a)|16)
    return 2,0

def create(uc,sp):
    i=slot(sp);trace.append(('projectile',i))
    short(unit+12+i*28+0x1a,word(unit+12+i*28+0x1a)&0xff0f)
    return 2,0

p.hooks.update({0x52fe30:start,0x52fff0:gate,0x530140:fire,0x530220:create})
p.freeze_hooks();put(0x62d55c,game);put(game+0x19f44,100);put(unit+0xb4,kind)
p.uc.mem_write(unit+0xf4,struct.pack('<f',1.0))
p.uc.mem_write(unit+0xd8,struct.pack('<f',100.0))
for i,w in enumerate(weapons):
    put(w+0x40,w)
    p.uc.mem_write(w+0xd4,struct.pack('<f',0.0))
rng=random.Random(0x52ae90)
rows=[];outputs=[]
for case in range(4096):
    enabled=rng.choice((True,True,False));selected=rng.randrange(4)
    put(kind+0x260,0x10000 if enabled else 0)
    put(unit+0x130,selected<<30);put(unit+0xcc,0)
    starts=[rng.randrange(2) for _ in range(3)]
    ready=[rng.randrange(2) for _ in range(3)]
    signal=[rng.randrange(2) for _ in range(3)]
    flags=[rng.randrange(65536) for _ in range(3)]
    reloads=[rng.choice((0,1,2,65535)) for _ in range(3)]
    present=[rng.randrange(2) for _ in range(3)]
    rows.append(" ".join(map(str,[int(enabled),selected,*[v for i in range(3) for v in (flags[i],reloads[i],present[i],starts[i],ready[i],signal[i])]])))
    expected=[]
    for i,w in enumerate(weapons):
        rec=unit+12+i*28
        put(rec,w if present[i] else 0);short(rec+0x14,reloads[i]);short(rec+0x1a,flags[i])
        if not enabled or not present[i] or selected not in (i,3):continue
        reloads[i]=max(0,reloads[i]-1)
        expected.append(('aim',i))
        if not starts[i]:continue
        expected.append(('ready',i))
        if ready[i]:
            expected.append(('fire_callback',i))
            if signal[i]:flags[i]|=16
        if flags[i]&16:
            expected.append(('projectile',i));flags[i]&=0xff0f
    trace.clear()
    _,error=p.call(0x52ae90,(unit,));assert not error,error
    assert trace==expected,(case,trace,expected)
    for i in range(3):
        assert word(unit+12+i*28+0x14)==reloads[i]
        assert word(unit+12+i*28+0x1a)==flags[i]
    assert read(unit+0xcc)==(700 if any(t[0]=='projectile' for t in expected) else 0)
    encoded=0
    codes={'aim':1,'ready':2,'fire_callback':3,'projectile':4}
    for action,i in trace:encoded=encoded*16+i*4+codes[action]
    outputs.append((encoded,read(unit+0xcc),*[v for i in range(3) for v in (flags[i],reloads[i])]))

print('PASS: 4096 native common weapon updates; selected/all slots, reloads, aim admission, FireWeapon callback and independent SET-23 projectile trigger')

# A delayed fire script can acknowledge on a later update, when the aim gate
# no longer reports ready. The native loop checks SET 23 independently of that gate.
put(kind+0x260,0x10000);put(unit+0x130,0);put(unit+12,weapons[0])
short(unit+12+0x1a,0);short(unit+12+0x14,0)
starts=[1,0,0];ready=[1,0,0];signal=[0,0,0]
trace.clear()
_,error=p.call(0x52ae90,(unit,));assert not error,error
assert trace==[('aim',0),('ready',0),('fire_callback',0)]
ready[0]=0;trace.clear()
_,error=p.call(0x52ae90,(unit,));assert not error,error
assert trace==[('aim',0),('ready',0)]
short(unit+12+0x1a,word(unit+12+0x1a)|16);trace.clear()
_,error=p.call(0x52ae90,(unit,));assert not error,error
assert trace==[('aim',0),('ready',0),('projectile',0)]
print('PASS: delayed script fire acknowledgement creates the projectile on a later update without another FireWeapon callback')

# Delayed acknowledgements survive lack of admission and deselection. Execute
# the real SET dispatcher and target retirement; empty callback lookup provides
# no implicit cancellation. An explicit SET 21 is tested separately.
script,host,definition=[HEAP+i*0x10000 for i in (6,7,8)]
put(unit+0xbc,script);put(script+0xc,definition);put(definition+4,0)
put(script+0xa64,host);put(host+12,unit)
for pending_slot in range(3):
    for interruption in ('target_lost','deselected','explicit_clear'):
        starts=[0,0,0];ready=[0,0,0];signal=[0,0,0]
        for i,w in enumerate(weapons):
            put(unit+12+i*28,w);short(unit+12+i*28+0x1a,i);short(unit+12+i*28+0x14,20)
        record=unit+12+pending_slot*28
        put(unit+0x130,pending_slot<<30);put(record+4,1)
        if interruption=='target_lost':
            _,error=p.call(0x51a7f0,(unit,pending_slot));assert not error,error
        _,error=p.call(0x50d450,(23,pending_slot),ecx=script);assert not error,error
        if interruption=='explicit_clear':
            _,error=p.call(0x51a7f0,(unit,pending_slot));assert not error,error
            assert word(record+0x1a)&16
            _,error=p.call(0x50d450,(21,pending_slot),ecx=script);assert not error,error
        selected=(pending_slot+1)%3 if interruption=='deselected' else pending_slot
        put(unit+0x130,selected<<30);trace.clear()
        _,error=p.call(0x52ae90,(unit,));assert not error,error
        assert trace==[('aim',selected)]
        assert bool(word(record+0x1a)&16)==(interruption!='explicit_clear')
        put(unit+0x130,pending_slot<<30);put(record+4,1);starts[pending_slot]=1;trace.clear()
        _,error=p.call(0x52ae90,(unit,));assert not error,error
        expected=[('aim',pending_slot),('ready',pending_slot)]
        if interruption!='explicit_clear':expected.append(('projectile',pending_slot))
        assert trace==expected,(pending_slot,interruption,trace)
        assert not word(record+0x1a)&16
print('PASS: 9 native late-acknowledgement timelines across target loss, deselection and explicit SET 21 cancellation')

# Follow one unit through successive selections: inactive reloads retain their
# exact value, then resume when selected; mode 3 clocks all present slots.
starts=[0,0,0];ready=[0,0,0];signal=[0,0,0]
expected_reload=[4,5,6]
for i,w in enumerate(weapons):
    put(unit+12+i*28,w);short(unit+12+i*28+0x14,expected_reload[i]);short(unit+12+i*28+0x1a,0)
for selected in (1,2,0,0,1,3):
    put(unit+0x130,selected<<30);trace.clear()
    _,error=p.call(0x52ae90,(unit,));assert not error,error
    slots=list(range(3)) if selected==3 else [selected]
    for i in slots:expected_reload[i]=max(0,expected_reload[i]-1)
    assert trace==[('aim',i) for i in slots],(selected,trace)
    assert [word(unit+12+i*28+0x14) for i in range(3)]==expected_reload
print('PASS: native weapon switching freezes inactive reloads and resumes them on reselection; all-slot mode advances every present weapon')

result=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/retail_visual_test','--weapon-update'],
    input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
assert len(actual)==len(outputs),(len(actual),len(outputs))
for i,(a,e) in enumerate(zip(actual,outputs)):
    assert a==e,(i,rows[i],a,e)
print('PASS: shared C++ weapon-update core matches all 4096 native update traces and states')
