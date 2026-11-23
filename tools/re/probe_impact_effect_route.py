#!/usr/bin/env python3
"""Native impact explosion dispatch: authored class, contact XYZ, water/direct hit.

Runs 529c10. Existing controlled damage/map services are reused; 492c80 is a
creation sink. No renderer or game launch. Missing class is native -1.
"""
import struct
from emu import HEAP, Icd
from probe_ballistic_impact_body import make_probe, put

freeze = Icd.freeze_hooks
Icd.freeze_hooks = lambda self: None
cases = 0
for water in (False, True):
    for victim in (False, True):
        for land_class, water_class in ((-1,-1),(3,-1),(-1,7),(3,7)):
            for enabled in (0,1):
                p, (game, shot, units, stride), logs = make_probe(0,0,1,units=((256,256,1),))
                weapon = HEAP + 2*0x10000
                cell = HEAP + 8*0x10000 + (16*64+16)*14
                p.uc.mem_write(game+0x19ef8, bytes([20]))
                p.uc.mem_write(cell+5, bytes([10 if water else 30]))
                put(p.uc,weapon+0x6c,land_class);put(p.uc,weapon+0x70,water_class)
                put(p.uc,weapon+0x74,123)
                position=(256*65536,193*65536+123,256*65536)
                p.uc.mem_write(shot+4,struct.pack('<3i',*position))
                created=[]
                def capture(uc,sp):
                    point,kind,arg=struct.unpack('<3I',uc.mem_read(sp,12))
                    created.append((kind,arg,struct.unpack('<3i',uc.mem_read(point,12))))
                    return 3,0
                p.hooks[0x492c80]=capture
                freeze(p)
                _,error=p.call(0x529c10,(shot,units+stride if victim else 0,0,enabled,0))
                assert error is None,error
                selected=water_class if water and not victim else land_class
                expected=[] if selected==-1 or not enabled else [(selected,123,position)]
                assert created==expected,(water,victim,land_class,water_class,enabled,created,expected)
                cases+=1
print(f'PASS: {cases} native impact explosion routes; exact contact XYZ; no missing-class substitution; water class only for environmental water hit')
