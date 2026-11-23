#!/usr/bin/env python3
"""Native damage-flame batch admission, variant choice and list construction.
Only allocation and CRT random are replaced; animation initialization is real.
"""
import struct
from emu import Icd, HEAP
p=Icd()
emitter,head,origin,pool=HEAP,HEAP+0x1000,HEAP+0x2000,HEAP+0x10000
allocated=[];random_value=0;draws=0
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def random(uc,sp):
    global draws
    draws+=1
    return 0,random_value
def allocate(uc,sp):
    assert read(sp)==36
    address=pool+len(allocated)*64
    allocated.append(address)
    return 1,address
p.hooks.update({0x4f3b50:allocate,0x5d4444:random})
p.freeze_hooks()
for i,v in enumerate((1234567,0x87654321,0xfedcba98)):put(origin+i*4,v)
for kind in range(3):
    table=HEAP+0x20000+kind*0x1000
    put(0x640258+kind*8,kind+1);put(0x64025c+kind*8,table)
    for variant in range(kind+1):
        animation=HEAP+0x30000+kind*0x1000+variant*0x100
        put(table+variant*4,animation)
        p.uc.mem_write(animation,struct.pack('<HB',4,0))
        for frame in range(4):
            put(animation+0x28+frame*8,1234+frame)
            p.uc.mem_write(animation+0x2c+frame*8,struct.pack('<H',variant+2))
cases=0
for current in (0,1,39,40):
    for requested in (0,1,2,40,50):
        for kind in range(3):
            for random_value in (0,1,16384,32767):
                allocated.clear();draws=0
                put(emitter+8,head);put(emitter+12,current);put(emitter+16,40)
                put(head,head);put(head+4,head)
                _,error=p.call(0x4f26a0,(requested,origin,kind),ecx=emitter)
                assert not error,error
                count=min(requested,40-current);variant=random_value*(kind+1)//32768
                assert len(allocated)==count and draws==count
                assert read(emitter+12)==current+count
                cursor=read(head)
                for address in allocated:
                    assert cursor==address
                    data=address+8
                    assert bytes(p.uc.mem_read(data,12))==bytes(p.uc.mem_read(origin,12))
                    assert struct.unpack('<HHB',p.uc.mem_read(data+12,5))==(0,variant+2,0)
                    assert read(data+20)==HEAP+0x30000+kind*0x1000+variant*0x100
                    assert read(data+24)==15
                    cursor=read(cursor)
                assert cursor==head
                cases+=1
print(f'PASS: {cases} native damage-flame emissions preserve capacity, variant selection, RNG count, authored initial frame and list order')
