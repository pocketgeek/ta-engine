#!/usr/bin/env python3
"""Probe retail action-mode 14 (armed build placement / FindSite cursor).

Runs the real 0x4dd780 selector. The fixture models a live selected unit and
tests the type build-list and unit context pointers read by the mode-14 branch.
"""
import struct
from emu import HEAP, Icd

SELECTOR=0x4DD780
NORMAL_SLOT=19
FINDSITE_SLOT=16

def put(uc,address,value):
    uc.mem_write(address,struct.pack('<I',value&0xffffffff))

icd=Icd();uc=icd.uc
unit,unit_type,map_obj,point=[HEAP+i*0x10000 for i in range(1,5)]
put(uc,unit+0x130,0x01000000) # live / not invalidated
put(uc,unit+8,1)             # native unit context present
put(uc,unit+0xb4,unit_type)
put(uc,unit+0xb8,map_obj)
put(uc,unit_type+0x132,1)    # native build-options list present

cases=(
    (0x01000000,1,1,FINDSITE_SLOT,'live builder with build list -> FindSite'),
    (0x01000000,0,1,NORMAL_SLOT,'no build list -> Normal'),
    (0x01000000,1,0,NORMAL_SLOT,'no unit context -> Normal'),
    (0,1,1,NORMAL_SLOT,'dead/ineligible unit -> Normal'),
)
for status,build_list,unit_context,expected,label in cases:
    put(uc,unit+0x130,status)
    put(uc,unit+8,unit_context)
    put(uc,unit_type+0x132,build_list)
    got,error=icd.call(SELECTOR,args=(14,unit,0,point))
    assert error is None,(label,error)
    assert got==expected,(label,got,expected)
    print(f'{label}: native cursor slot {got}')

print('PASS: native mode 14 returns slot 16 only for a live unit with both build-list and unit-context pointers')
