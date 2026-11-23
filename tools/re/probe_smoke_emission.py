#!/usr/bin/env python3
"""Execute native smoke admission, frame-limit selection and list insertion.

Only allocation, RNG and animation frame-count lookup are substituted.
"""
import struct
from emu import Icd, HEAP
p=Icd()
emitter,head,origin,game,pool=[HEAP+i*0x10000 for i in range(5)]
random_value=0
allocated=[]
lookups=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
def allocate(uc,sp):
    assert read(sp)==40
    address=pool+len(allocated)*64
    allocated.append(address)
    return 1,address
def frames(uc,sp):
    bank=read(sp);lookups.append(bank)
    return 1,bank
p.hooks.update({0x4f3a00:allocate,0x5d4444:lambda uc,sp:(0,random_value),0x5367d0:frames})
p.freeze_hooks()
put(0x62d55c,game)
for offset,count in ((0x174a0,16),(0x174a4,12),(0x174e4,20)):put(game+offset,count)
for i,v in enumerate((0x12345678,0xabcdef01,0xfedcba98)):put(origin+i*4,v)
cases=0
for current in (0,1,9,10):
    for requested in (0,1,2,10,20):
        for period in (0,1,8,17):
            for flags in ((0,0),(1,0),(0,1),(1,1)):
                for random_value in (0,1,16384,32767):
                    allocated.clear();lookups.clear()
                    put(emitter+8,head);put(emitter+12,current);put(emitter+16,10)
                    put(head,head);put(head+4,head)
                    _,error=p.call(0x4f1d40,(requested,origin,period,*flags),ecx=emitter)
                    assert not error,error
                    count=min(requested,10-current)
                    bank=20 if flags[1] else (12 if flags[0] else 16)
                    assert len(allocated)==count and lookups==[bank]*count
                    assert read(emitter+12)==current+count
                    cursor=read(head)
                    for address in allocated:
                        assert cursor==address
                        data=address+8
                        assert bytes(p.uc.mem_read(data+1,12))==bytes(p.uc.mem_read(origin,12))
                        assert read(data+13)==(period or 8)
                        assert read(data+17)==2+random_value*(bank-3)//32768
                        assert read(data+21)==(period or 8) and read(data+25)==0
                        assert p.uc.mem_read(data,1)[0]==flags[0]
                        assert p.uc.mem_read(data+29,1)[0]==flags[1]
                        cursor=read(cursor)
                    assert cursor==head
                    cases+=1
print(f'PASS: {cases} native smoke emissions preserve capacity, bank choice, randomized lifetime, period and list order')
