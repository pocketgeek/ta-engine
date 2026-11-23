"""Identify the fixed-point trig helpers 0x4b70ef / 0x4b7123 (cdecl (angle, radius))."""
import sys; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
from ai_emu import *
e = Emu()
for ang in (0, 0x2000, 0x4000, 0x8000, 0xc000):
    a = e.call(0x4b70ef, (ang, 0x10000)); b = e.call(0x4b7123, (ang, 0x10000))
    s = lambda v: struct.unpack('<i', struct.pack('<I', v))[0] / 65536
    print(hex(ang), 'f1=%.4f f2=%.4f' % (s(a), s(b)))
