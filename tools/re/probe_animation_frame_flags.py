#!/usr/bin/env python3
"""Native animation-bank relocation preserves distinct subframe/flag bytes.

Runs complete 537490 with only its file-read dependency substituted. Synthetic
bank memory uses real on-disk offsets; every relocated pointer is checked.
"""
import struct
from emu import Icd, HEAP
p=Icd();bank=HEAP
p.hooks[0x53b6f0]=lambda uc,sp:(2,bank)
p.freeze_hooks()
for flag in range(256):
    for count in (0,1,2,255):
        data=bytearray(0x6000)
        struct.pack_into('<3I',data,0,0x10100,1,0)
        struct.pack_into('<I',data,12,0x100)
        struct.pack_into('<H',data,0x100,1)
        data[0x102]=flag
        struct.pack_into('<2I',data,0x128,0x200,2)
        data[0x209:0x20c]=bytes((4,count,flag))
        struct.pack_into('<I',data,0x210,0x300)
        for i in range(count):
            off=0x800+i*24
            struct.pack_into('<I',data,0x300+i*4,off)
            data[off+9:off+12]=bytes((4,0,flag))
            struct.pack_into('<I',data,off+16,0x5000+i*2)
        p.uc.mem_write(bank,bytes(data))
        result,error=p.call(0x537490,(bank+0x5800,))
        assert not error,(flag,count,error)
        assert result==bank
        actual=bytes(p.uc.mem_read(bank,len(data)))
        def ptr(off):return struct.unpack_from('<I',actual,off)[0]
        assert actual[0x102]==flag, ('sequence loop flag',flag,count)
        assert ptr(12)==bank+0x100 and ptr(0x128)==bank+0x200
        assert ptr(0x210)==bank+0x300
        assert actual[0x209:0x20c]==bytes((4,count,flag))
        for i in range(count):
            off=0x800+i*24
            assert ptr(0x300+i*4)==bank+off and ptr(off+16)==bank+0x5000+i*2
            assert actual[off+9:off+12]==bytes((4,0,flag))
print('PASS: 1024 native bank loads keep the one-byte subframe count independent of all 256 blend flags and preserve both through relocation')

print('PASS: all authored sequence loop bytes survive native animation-bank relocation')
