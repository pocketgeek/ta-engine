#!/usr/bin/env python3
"""Observe native truecolor sprite renderer state at quad submission.

Executes full 4fac00 for synthetic format-4/5 frames. Renderer acquisition,
state-setting and primitive submission are sinks. This tests state selection,
not GPU raster arithmetic or the loader that produces runtime frame flags.
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p=Icd()
frame,texture,device,vtable=[HEAP+i*0x10000 for i in range(4)]
state={};modes=[];draws=[]
def put(address,value):p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
def set_state(uc,sp):
    key,value=struct.unpack('<2I',uc.mem_read(sp,8));state[key]=value
    return 2,0
def mode(uc,sp):
    modes.append(struct.unpack('<I',uc.mem_read(sp,4))[0]);return 1,0
def draw(uc,sp):
    primitive,vertices,count=struct.unpack('<3I',uc.mem_read(sp,12))
    draws.append((primitive,count,dict(state),list(modes)))
    return 3,0
def textured_quad(uc,sp):
    handle,vertices=struct.unpack('<2I',uc.mem_read(sp,8))
    assert handle==123
    draws.append((6,4,dict(state),list(modes)))
    return 2,0
p.hooks.update({0x5ac2f0:lambda uc,sp:(0,device),0x5ac370:lambda uc,sp:(0,device),
    0x5ac3a0:lambda uc,sp:(0,0),HEAP+0x50000:set_state,HEAP+0x50010:draw,
    HEAP+0x50040:textured_quad,HEAP+0x50020:mode,HEAP+0x50030:lambda uc,sp:(1,0)})
p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
put(device,vtable);put(vtable+0x6c,HEAP+0x50000);put(vtable+0x64,HEAP+0x50010)
put(vtable+0x48,HEAP+0x50040);put(vtable+0x50,HEAP+0x50020);put(vtable+0x54,HEAP+0x50030)
p.uc.mem_write(frame,struct.pack('<4h',16,12,3,4));put(frame+0xc,texture)
p.uc.mem_write(texture,struct.pack('<3I',123,32,16))
for encoding in (4,5):
    for flag in range(256):
        for special in (0,1,255):
            p.uc.mem_write(frame+9,bytes((encoding,0,flag)))
            state.clear();modes.clear();draws.clear()
            _,error=p.call(0x4fac00,(frame,100,200,special,0,0))
            assert not error,(encoding,flag,special,error)
            assert len(draws)==1,(encoding,flag,special,draws)
            primitive,count,at_draw,at_modes=draws[0]
            assert (primitive,count)==(6,4)
            if encoding==4:
                assert at_draw[27]==1 and at_draw[19]==5
                assert at_draw[20]==(6 if flag==255 else 2),(flag,special,at_draw)
                assert state[19]==5 and state[20]==6 # restore default after draw
            else:
                assert at_modes==[8,5 if special and flag!=255 else 4],(flag,special,at_modes)
print('PASS: 1536 native truecolor sprite draws select format-4 source/destination states by runtime frame flag; format-5 mode also depends on the call flag')

# Composite recursion retains one mutable special-call flag across siblings.
# A nonzero child flag enables it unless the caller supplied an override.
children=HEAP+0x60000
pointers=HEAP+0x70000
p.uc.mem_write(frame+9,bytes((0,3,0)))
put(frame+0x10,pointers)
composites=0
for flags in ((0,0,0),(1,0,0),(0,1,0),(0,0,1),(255,0,1),(0,255,0)):
    for encodings in ((5,5,5),(4,5,4),(5,4,5)):
        for special in (0,1):
            for override in (0,1):
                for i,(flag,encoding) in enumerate(zip(flags,encodings)):
                    child=children+i*32
                    put(pointers+i*4,child)
                    p.uc.mem_write(child,struct.pack('<4h',16,12,i,4))
                    p.uc.mem_write(child+9,bytes((encoding,0,flag)))
                    put(child+0xc,texture)
                state.clear();modes.clear();draws.clear()
                _,error=p.call(0x4fac00,(frame,100,200,special,override,128))
                assert not error,(flags,encodings,special,override,error)
                assert len(draws)==3
                active=special
                previous_modes=0
                for i,(flag,encoding) in enumerate(zip(flags,encodings)):
                    if flag and not override:active=1
                    _,_,at_draw,at_modes=draws[i]
                    if encoding==4:
                        assert at_draw[19]==5 and at_draw[20]==(6 if flag==255 else 2)
                        assert len(at_modes)==previous_modes
                    else:
                        assert at_modes[previous_modes:]==[8,5 if active and flag!=255 else 4]
                        previous_modes=len(at_modes)
                composites+=1
print(f'PASS: {composites} native composite draws preserve child blend selection, sibling order and sticky special flags with caller override')

# Indexed textures use mode 4 normally, mode 5 for the special call flag,
# or mode 6 for explicit alpha when the special call flag is clear.
p.uc.mem_write(frame+9,bytes((1,0,0)))
put(frame+0xc,texture)
indexed=0
for flag in range(256):
    for special in (0,1):
        for override in (0,1):
            p.uc.mem_write(frame+11,bytes((flag,)))
            state.clear();modes.clear();draws.clear()
            _,error=p.call(0x4fac00,(frame,100,200,special,override,73))
            assert not error,error
            assert len(draws)==1
            assert modes==[8,5 if special else (6 if override else 4)],(flag,special,override,modes)
            indexed+=1
print(f'PASS: {indexed} indexed sprite draws select opaque, half-alpha or explicit-alpha modes')
