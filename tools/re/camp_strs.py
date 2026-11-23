"""Dump all strings in a VA range with their code xrefs (campaign RE)."""
import sys, re; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
a = lo
while a < hi:
    d = b.rd(a, 1)[0]
    if 32 <= d < 127:
        s = b.cstr(a)
        if len(s) >= 2 and all(32 <= ord(c) < 127 or c in '\r\n\t' for c in s):
            print(hex(a), repr(s), [hex(x) for x in b.xrefs(a)][:8])
        a += len(s) + 1
    else:
        a += 1
