#!/usr/bin/env python3
"""Observe native pickup queue removal and sleeping-passenger cancellation.

Executes queue removal, mission destruction, target-reference unlinking and the
real passenger dispatcher/handler. Controller destruction, model notifications
and allocator release are recorded sinks. No World parity is claimed here.
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p=Icd()
carrier,passenger,cm,pm,kind,owner,game,defs,mover,nav,controller=[HEAP+i*0x10000 for i in range(11)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def byte(a,v):p.uc.mem_write(a,bytes([v]))
released=[];notifications=[];detached=[]
def free(uc,sp):released.append(read(sp));return 0,0

def notify(uc,sp):
    notifications.append((uc.reg_read(UC_X86_REG_ECX),read(sp)))
    return 1,0

def detach(uc,sp):
    m=uc.reg_read(UC_X86_REG_ECX)
    assert read(sp)==0
    detached.append(m);put(m+0x6e,0)
    return 1,0

p.hooks.update({0x4eba00:free,0x519b10:notify,0x4d4d40:detach,
                0x519f50:lambda uc,sp:(1,1)})
p.freeze_hooks()
put(0x62d55c,game);put(0x62db84,defs);put(defs+25+4,0x403430)
put(passenger+0xb4,kind);put(passenger+0xb8,owner)
put(passenger+0x130,0x1000000)
byte(kind+0x24b,0) # no automatic idle mission factory
put(owner,1);byte(owner+0xea,1)
count=0
for mask in (0,0x709,0x729,0x89,0x789):
    for has_controller in (False,True):
        for queued in (False,True):
            p.uc.mem_write(cm,bytes(0x200));p.uc.mem_write(pm,bytes(0x80))
            for m,u,target in ((cm,carrier,passenger),(pm,passenger,carrier)):
                byte(m+4,1);put(m+0xe,u);put(m+0x16,target)
                put(target+0xc4,m+0x12) # actual target-reference chain
            put(cm+6,mask)
            put(carrier+0x60,cm+0x100 if queued else cm)
            if queued:put(cm+0x100+0x66,cm)
            put(carrier+8,mover if has_controller else 0)
            put(mover,nav);put(nav+4,controller)
            put(cm+0x6e,controller if has_controller else 0)
            put(passenger+0x60,pm);put(passenger+0xd0,0x100)
            passenger_before=bytes(p.uc.mem_read(pm,0x80))
            released.clear();notifications.clear();detached.clear()
            _,error=p.call(0x4d6ad0,(carrier,cm));assert not error,error
            assert released==[cm]
            assert detached==([cm] if has_controller else [])
            assert notifications==([] if queued else [(carrier,3)])
            assert read(passenger+0x60)==pm and read(passenger+0xd0)==0x100
            assert bytes(p.uc.mem_read(pm,0x80))==passenger_before
            assert read(passenger+0xc4)==0 and read(cm+0x16)==0
            assert read(carrier+0xc4)==pm+0x12
            assert read(carrier+0x60)==(cm+0x100 if queued else 0)
            count+=1
print(f'PASS: {count} native pickup removals leave reciprocal passenger mission/events intact and unlink only the retiring target reference')

# Carrier pickup has vanished, but the actual dispatcher must not invoke the
# sleeping passenger handler until its mask has an event or deadline expires.
for event in (0,8,0x80,0x100):
    p.uc.mem_write(pm,bytes(0x80))
    byte(pm+4,1);byte(pm+5,1);put(pm+0xe,passenger);put(pm+0x16,carrier)
    put(pm+6,0x89);put(pm+0xa,130)
    put(carrier+0x60,0);put(carrier+0xc4,pm+0x12)
    put(passenger+0x60,pm);put(passenger+0xd0,event)
    released.clear();notifications.clear();detached.clear()
    put(game+0x19f44,101)
    _,error=p.call(0x4d8450,(passenger,));assert not error,error
    wakes=bool(event&0x89)
    assert read(passenger+0x60)==(0 if wakes else pm)
    assert released==([pm] if wakes else [])
    if not wakes:
        put(game+0x19f44,130)
        _,error=p.call(0x4d8450,(passenger,));assert not error,error
        assert read(passenger+0x60)==0 and released==[pm]
print('PASS: native passenger cancellation waits for a subscribed event or deadline; unrelated navigator arrival does not wake the air wait')

# Execute the actual queue insertion used by opportunistic pickup, then retire
# that child. The interrupted mission must retain its request data, with only
# stage/wait reset by ordinary (non-preserving) inserted mission flags.
child=HEAP+0xb0000
put(carrier+0xb8,owner);put(carrier+0xb4,kind);put(carrier+0x130,0x1000000)
put(carrier+8,0)
count=0
for stage in (0,1,2,3):
    for mask in (0,0x709,0x729,0x89,0x789):
        for child_flags in (0,0x20,0x40,0x60):
            for inherited in (0,0x2000):
                p.uc.mem_write(cm,bytes(0x80));p.uc.mem_write(child,bytes(0x80))
                byte(cm+4,1);byte(cm+5,stage);put(cm+6,mask);put(cm+0xa,777)
                put(cm+0xe,carrier);put(cm+0x4e,3);put(cm+0x52,11);put(cm+0x5a,inherited)
                byte(child+4,1);put(child+0x5a,child_flags);put(carrier+0x60,cm)
                before=bytearray(p.uc.mem_read(cm,0x80))
                _,error=p.call(0x4d7750,(carrier,child));assert not error,error
                assert read(carrier+0x60)==child and read(child+0x66)==cm
                assert read(child+0xe)==carrier and read(child+0x5a)==child_flags|inherited
                if not child_flags&0x60:
                    before[5]=0;before[6:10]=bytes(4)
                assert bytes(p.uc.mem_read(cm,0x80))==bytes(before),(stage,mask,child_flags,inherited)
                released.clear();notifications.clear();detached.clear()
                _,error=p.call(0x4d6ad0,(carrier,child));assert not error,error
                assert read(carrier+0x60)==cm and released==[child]
                assert bytes(p.uc.mem_read(cm,0x80))==bytes(before)
                assert notifications==[(carrier,3)]
                count+=1
print(f'PASS: {count} native interruption/resumption chains preserve the interrupted mission data, reset stage/wait for ordinary inserted missions, and restore the same mission after child removal')
