#!/usr/bin/env python3
"""Native weapon target lookup, pending-death rejection and target retirement.

51a9a0 and 51a7f0 execute; only the TargetCleared script callback is recorded.
This tests lookup/cleanup, not the surrounding mission or script phase order.
"""
import struct
from emu import Icd,HEAP
p=Icd();units,kind,game=[HEAP+i*0x10000 for i in range(3)]
owner=units+312
calls=[]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def short(a,v):p.uc.mem_write(a,struct.pack('<H',v&0xffff))
def callback(uc,sp):
    args=struct.unpack('<8I',uc.mem_read(sp,32))
    assert bytes(uc.mem_read(args[0],14))==b'TargetCleared\0'
    assert args[3]==1
    calls.append(args[4]);return 8,0
p.hooks[0x56c640]=callback;p.freeze_hooks()
put(0x62d55c,game);put(game+0x14e84,units);put(game+0x14e88,units+4*312)
put(owner+0xb4,kind)
count=0
for enabled in (False,True):
    put(kind+0x260,0x10000 if enabled else 0)
    for selected in range(4):
        put(owner+0x130,0x1000000|(selected<<30))
        for slot in range(3):
            record=owner+12+slot*28
            for target_id in (0,2,4,5):
                target=units+target_id*312
                for height in (0x8000,123):
                    for flags in (0,0x1000,0x1000000,0x1001000):
                        put(target+0x130,flags)
                        p.uc.mem_write(record+4,struct.pack('<2H',target_id,height))
                        short(record+0x1a,0xf8|slot);calls.clear()
                        result,error=p.call(0x51a9a0,(owner,slot));assert not error,error
                        lookup=(not enabled or selected==3 or selected==slot) and height==0x8000 and target_id!=0
                        live=target_id<=4 and flags==0x1000000
                        retired=lookup and not live
                        assert result==(target if lookup and live else 0),(enabled,selected,slot,target_id,height,flags,result)
                        assert calls==([slot] if retired else [])
                        expected=(0,0x8000) if retired else (target_id,height)
                        assert bytes(p.uc.mem_read(record+4,4))==struct.pack('<2H',*expected)
                        assert struct.unpack('<H',p.uc.mem_read(record+0x1a,2))[0]==0xf8|slot
                        count+=1
print(f'PASS: {count} native target lookups; selected/all slots, ground versus unit references, bounds, pending-death rejection and immediate target retirement')
