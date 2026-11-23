#!/usr/bin/env python3
"""Check imported factory handshake flags against original COB unit getters.

These are unit-side values, separate from the saved script threads and pieces.
Uses synthetic records; no retail assets or captured state are written.
"""
import base64
import struct
import zlib

from emu import Icd, HEAP
from probe_saved_movement import factory_unit_values


def main():
    p=Icd()
    vm,model,unit=HEAP,HEAP+0x10000,HEAP+0x20000
    p.uc.mem_write(vm+0xa64,struct.pack('<I',model))
    p.uc.mem_write(model+0xc,struct.pack('<I',unit))
    cases=0
    for activation in (0,1,0xfe,0xff):
        for flags in range(256):
            data=bytearray(0x138)
            data[0x114]=activation
            data[0x12f]=flags
            p.uc.mem_write(unit,bytes(data))
            runtime={'units':[{'id':7,'address':unit}],
                     'entity_pool':{'address':unit,'size':len(data),
                         'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}}
            expected=[]
            for query in (1,5,18,19):
                result,error=p.call(0x50ceb0,(query,0,0,0,0),ecx=vm)
                if error: raise RuntimeError(error)
                expected.append(result)
            actual=factory_unit_values(runtime)[7][:4]
            if actual!=expected: raise AssertionError((activation,flags,expected,actual))
            cases+=1
    print(f'PASS: {cases} factory activation/readiness/yard flags match original unit getters')


if __name__=='__main__': main()
