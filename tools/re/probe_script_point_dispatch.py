#!/usr/bin/env python3
"""Check native point-effect endpoint selection after model refresh.

Executes full COB effect dispatch, sinking visibility, pose refresh and the
owner emitter. Input vertices represent the refreshed native model pose.
"""
import random
import struct
from emu import Icd, HEAP
p = Icd()
vm, view, unit, game, vertices = [HEAP+i*0x10000 for i in range(5)]
calls = []
refreshes = []
visible = True
pose = bytes(24)
def put(a,v): p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def refresh(uc,sp):
    refreshes.append(1)
    uc.mem_write(vertices,pose)
    return 1,0

def emit(uc,sp):
    a,b,period,owner = struct.unpack('<4I',uc.mem_read(sp,16))
    calls.append((struct.unpack('<3I',uc.mem_read(a,12)),
                  struct.unpack('<3I',uc.mem_read(b,12)),period,owner))
    return 4,0
p.hooks.update({0x4f7210:lambda uc,sp:(2,int(visible)),
                0x4ee620:refresh,0x502aa0:emit})
p.freeze_hooks()
put(vm+0xa64,view);put(view+0xc,unit);put(0x62d55c,game)
rng = random.Random(0x50dacc)
for case in range(1024):
    body = [rng.getrandbits(32) for _ in range(3)]
    points = [[rng.getrandbits(32) for _ in range(3)] for _ in range(2)]
    pose = struct.pack('<6I',*points[0],*points[1])
    piece = case % 8
    put(view+0x1f0+piece*56,vertices)
    p.uc.mem_write(unit+0x68,struct.pack('<3I',*body))
    endpoints = [tuple((body[i]+v[i]*(1 if i<2 else -1))&0xffffffff
                       for i in range(3)) for v in points]
    for code in (2,3,4,5):
        visible = case % 3 != 0
        calls.clear();refreshes.clear()
        p.uc.mem_write(vertices,bytes(24))
        _,error = p.call(0x50da20,(piece,code),ecx=vm)
        assert not error,error
        if not visible:
            assert not calls and not refreshes
        else:
            a,b = endpoints if code<4 else endpoints[::-1]
            assert refreshes == [1]
            assert calls == [(a,b,16 if code%2==0 else 8,view)],(case,code,calls)
print('PASS: 4096 native point-effect dispatches use refreshed model vertices, reverse endpoints for 4/5, and select 16/8-tick cadence')
