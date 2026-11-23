#!/usr/bin/env python3
"""Observe extended COB effect dispatch and emission-time piece coordinates.

Execute 50da20 and smoke wrappers, sinking visibility, model refresh and final
particle/event creation. Does not establish particle lifetime or draw parity.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd()
vm,view,unit,game=[HEAP+i*0x10000 for i in range(4)]
calls=[]
visible=True
refreshes=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def coords(uc,a):return struct.unpack('<3I',uc.mem_read(a,12))
def smoke(uc,sp):
    args=struct.unpack('<8I',uc.mem_read(sp,32))
    calls.append(('smoke',coords(uc,args[1]),args[2:]))
    return 8,0
def damage(uc,sp):
    point,kind,owner=struct.unpack('<3I',uc.mem_read(sp,12))
    calls.append(('damage',coords(uc,point),kind))
    return 3,0
def transient(uc,sp):
    point,animation=struct.unpack('<2I',uc.mem_read(sp,8))
    calls.append(('transient',coords(uc,point),animation))
    return 2,0
def refresh(uc,sp):
    refreshes.append(1)
    return 1,0
def legacy(uc,sp):
    first,second,count,duration,group=struct.unpack('<5I',uc.mem_read(sp,20))
    calls.append(('legacy',coords(uc,first),coords(uc,second),count,duration,group))
    return 5,0
p.hooks.update({0x4f7210:lambda uc,sp:(2,int(visible)),0x4ee620:refresh,
                0x502660:smoke,0x502da0:damage,0x421e10:transient,0x502580:legacy})
p.freeze_hooks()
put(vm+0xa64,view);put(view+0xc,unit);put(0x62d55c,game)
put(game+0x174d4,111);put(game+0x174d8,222)
p.uc.mem_write(game+0x19ef8,b'\x17')
rng=random.Random(0x50da20)
for case in range(1024):
    body=[rng.getrandbits(32) for _ in range(3)]
    offset=[rng.getrandbits(32) for _ in range(3)]
    for i in range(3):
        put(unit+0x68+i*4,body[i]);put(view+0x1e4+i*4,offset[i])
    origin=((body[0]+offset[0])&0xffffffff,(body[1]+offset[1])&0xffffffff,
            (body[2]-offset[2])&0xffffffff)
    for code in range(257,266):
        visible=case%3!=0
        calls.clear();refreshes.clear()
        _,error=p.call(0x50da20,(0,code),ecx=vm)
        assert not error,(code,error)
        if not visible:
            assert not calls and not refreshes
            continue
        assert refreshes==[1]
        if code in (257,258,265):
            expected=('smoke',origin,(0,1,0,0,int(code==258),int(code==265)))
        elif code==259:
            assert calls==[],(case,code,calls) # real 502bd0 returns without creating anything
            continue
        elif code<=262:
            expected=('damage',origin,code-260)
        else:
            expected=('transient',origin,111 if code==263 else 222)
        assert calls==[expected],(case,code,calls,expected)
print('PASS: 9216 extended script emissions match native dispatch, visibility admission and emission-time transformed origins')

vertices=HEAP+0x40000
put(view+0x1f0,vertices)
for case in range(1024):
    body=[rng.getrandbits(32) for _ in range(3)]
    ends=[[rng.getrandbits(32) for _ in range(3)] for _ in range(2)]
    for axis in range(3):
        put(unit+0x68+axis*4,body[axis])
        for endpoint in range(2):put(vertices+endpoint*12+axis*4,ends[endpoint][axis])
    origins=[((body[0]+end[0])&0xffffffff,(body[1]+end[1])&0xffffffff,
              (body[2]-end[2])&0xffffffff) for end in ends]
    for code in (0,1):
        visible=case%3!=0;calls.clear();refreshes.clear()
        _,error=p.call(0x50da20,(0,code),ecx=vm)
        assert not error,(code,error)
        assert refreshes==([1] if visible else [])
        expected=[('legacy',*origins,1,6+code,7)] if visible else []
        assert calls==expected,(case,code,calls,expected)
print('PASS: 2048 legacy codes 0/1 dispatch transformed vertex endpoints to native emitter durations 6/7, group 7, count 1')
