"""Find E8 rel32 call sites targeting given VAs (observe only)."""
import sys, struct; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo, hi = b.text; seg = b.rd(lo, hi - lo)
for t in sys.argv[1:]:
    t = int(t, 16); out = []
    for i in range(len(seg) - 5):
        if seg[i] == 0xe8 and (lo + i + 5 + struct.unpack('<i', seg[i+1:i+5])[0]) & 0xffffffff == t:
            out.append(hex(lo + i))
    print(hex(t), out)
