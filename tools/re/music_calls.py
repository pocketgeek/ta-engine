"""Find direct E8/E9 call/jmp sites targeting given VAs in TotalA.exe, plus abs refs.
Usage: music_calls.py VA [VA...]"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo, hi = b.text
seg = b.rd(lo, hi - lo)
for a in sys.argv[1:]:
    t = int(a, 16); out = []
    for i in range(len(seg) - 5):
        if seg[i] in (0xe8, 0xe9):
            if (lo + i + 5 + struct.unpack_from('<i', seg, i + 1)[0]) & 0xffffffff == t:
                out.append(('call' if seg[i] == 0xe8 else 'jmp', hex(lo + i)))
    print(a, out, 'abs:', [hex(x) for x in b.xrefs(t)])
