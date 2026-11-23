#!/usr/bin/env python3
"""Run native debris update and removal with attached-emitter boundary sinks."""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p=Icd()
manager,debris,piece,game,emitter,vtable=[HEAP+i*0x10000 for i in range(6)]
api=HEAP+0x70000
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a,n=1):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
calls=[]
def emitter_update(uc,sp):
    assert uc.reg_read(UC_X86_REG_ECX)==emitter
    calls.append(('update',get(debris+0x2c,3)));return 0,0
def emitter_destroy(uc,sp):
    assert uc.reg_read(UC_X86_REG_ECX)==emitter and get(sp)==(1,)
    calls.append(('destroy',get(debris+0x2c,3)));return 1,0
def release_vector(uc,sp):
    calls.append(('release',uc.reg_read(UC_X86_REG_ECX)));return 0,0
height=0
p.hooks.update({api:emitter_destroy,api+16:emitter_update,
                0x4ebaa0:release_vector,0x511260:lambda uc,sp:(1,height)})
p.freeze_hooks();put(0x62d55c,game);put(emitter,vtable)
put(vtable,api);put(vtable+12,api+16)
checks=0
for attached in (False,True):
    for mode in ('expiry','water','ground','alive'):
        p.uc.mem_write(manager,bytes(0x3000));p.uc.mem_write(debris,bytes(0x220))
        put(manager+0x2b74,debris);put(debris+0x44,piece)
        put(debris+0x1d0,emitter if attached else 0)
        position=(100*65536,50*65536,100*65536);velocity=(123,456,789)
        put(debris+0x14,*velocity);put(debris+0x20,1 if mode=='expiry' else 900)
        put(debris+0x2c,*position)
        p.uc.mem_write(game+0x19ef8,bytes([60 if mode=='water' else 0]))
        height=60 if mode=='ground' else 0
        calls.clear();_,error=p.call(0x492420,ecx=manager);assert not error,error
        if mode=='alive':
            expected=[('update',tuple(a+b for a,b in zip(position,velocity)))] if attached else []
            assert get(manager+0x2b74)==(debris,)
        else:
            expected=([('destroy',position)] if attached else [])+[
                ('release',debris+0x74),('release',debris+0xc8)]
            assert get(manager+0x2b74)==(0,)
        assert calls==expected,(attached,mode,calls,expected)
        checks+=1
print(f'PASS: {checks} native debris lifecycles destroy attached emitters on expiry/water/ground before further particle updates; survivors update after movement')
