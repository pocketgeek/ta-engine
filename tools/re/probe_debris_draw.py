#!/usr/bin/env python3
"""Observe native debris admission and model-draw setup (492590).

Only viewport admission and the final model draw are sinks. This does not
establish final Glide raster parity or detached vertex transformation.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd()
manager,debris,piece,game,settings,options,sight,explored=[HEAP+i*0x40000 for i in range(8)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a,n):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
visible=True
calls=[]
def draw(uc,sp):
    unit=get(sp,1)[0]
    calls.append((unit,get(unit+0x68,3),bytes(uc.mem_read(unit+0x7c,6)),
                  get(unit+0xc0,1)[0],get(unit+0xb4,1)[0],uc.mem_read(unit+0xfd,1)[0]))
    return 1,0
p.hooks.update({0x48c870:lambda uc,sp:(1,int(visible)),0x4ee700:draw})
p.freeze_hooks()
put(0x62d55c,game);put(0x62d558,settings);put(settings+8,options)
put(game+0x19ef4,explored)
put(manager+0x2b74,debris);put(debris+0x44,piece)
put(debris+0x50+0x1c8,piece)
rng=random.Random(0x492590)
for case in range(2048):
    player=case%8;use_sight=(case//8)%2
    p.uc.mem_write(game+0x306f,bytes([player]));p.uc.mem_write(options+0x15,bytes([use_sight]))
    info=game+0x2404+player*272
    put(info+0x88,sight,64,64)
    x=rng.randrange(-32,2080);y=rng.randrange(-32,128);z=rng.randrange(-32,2080)
    position=(x*65536+123,y*65536+456,z*65536+789)
    put(debris+0x2c,*position)
    angles=[rng.randrange(65536) for _ in range(3)]
    p.uc.mem_write(piece+0x10,struct.pack('<3H',*angles))
    put(debris+0x48,0x12345000);put(debris+0x4c,case%8)
    visible=case%5!=0;sight_admitted=case%3!=0;explored_admitted=case%7!=0
    p.uc.mem_write(sight,bytes(4096));p.uc.mem_write(explored,bytes(8192))
    cx=x>>5;cz=(z-(y>>1))>>5
    inside=0<=cx<64 and 0<=cz<64
    if inside:
        p.uc.mem_write(sight+cz*64+cx,bytes([int(sight_admitted)]))
        mask=(1<<player) if explored_admitted else (1<<((player+1)%8))
        p.uc.mem_write(explored+2*(cz*64+cx),struct.pack('<H',mask))
    calls.clear()
    _,error=p.call(0x492590,ecx=manager)
    assert not error,error
    expected=visible and inside and (sight_admitted if use_sight else explored_admitted)
    assert bool(calls)==expected,(case,calls,expected)
    if expected:
        unit=manager+0x2d04
        assert calls==[(unit,tuple(v&0xffffffff for v in position),
                       struct.pack('<3H',angles[2],angles[1],angles[0]),
                       debris+0x50,0x12345000,case%8)],(case,calls)
        assert get(debris+0x50+0xc,1)==(unit,)
print('PASS: 2048 native debris draws: viewport/projected sight/exploration admission and detached model/body/rotation/color setup')
