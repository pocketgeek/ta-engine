#!/usr/bin/env python3
"""Execute the retail explosion manager with real authored sprite clocks.

Synthetic class/light data and no debris. No native routines are replaced.
Checks that sprite completion and light expiry independently control recycling.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd();p.freeze_hooks()
manager,effect,animation,game=[HEAP+i*0x40000 for i in range(4)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
put(0x62d55c,game)
rng=random.Random(0x492320)
checks=0
for case in range(256):
    durations=[rng.randrange(0,9) for _ in range(1+case%7)]
    life=sum(max(1,d) for d in durations)
    light=case%4;deadline=case%33;start=(0xfffffffa if case%4==0 else rng.getrandbits(32))
    p.uc.mem_write(effect,bytes(64));p.uc.mem_write(manager,bytes(0x3000))
    metadata=bytearray(0x30+8*len(durations))
    struct.pack_into('<H',metadata,0,len(durations))
    for i,d in enumerate(durations):struct.pack_into('<H',metadata,0x2c+i*8,d)
    p.uc.mem_write(animation,bytes(metadata))
    _,error=p.call(0x537390,(effect,animation,0));assert not error,error
    put(manager+0x2b5c,effect);put(effect+0x18,start,light)
    enabled=case%3!=0;p.uc.mem_write(effect+0x20,bytes([enabled]))
    put(0x6112b0+light*12,deadline)
    for elapsed in range(1,max(life,deadline+1)+2):
        put(game+0x19f44,start+elapsed)
        _,error=p.call(0x492320,ecx=manager);assert not error,error
        sprite_active=elapsed<life
        light_active=enabled and elapsed<=deadline
        assert bool(get(effect+8))==sprite_active,(case,elapsed,life)
        assert bool(p.uc.mem_read(effect+0x20,1)[0])==light_active
        retained=sprite_active or light_active
        assert get(manager+0x2b5c)==(effect if retained else 0)
        assert get(manager+0x2b60)==(0 if retained else effect)
        checks+=1
        if not retained:break
print(f'PASS: {checks} native explosion-manager updates: authored frame expiry, inclusive light deadline, wrapped elapsed time and recycling after both finish')
