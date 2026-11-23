"""Find call sites of given targets and code referencing a struct displacement (campaign RE).
usage: camp_xref.py call 0x48fed0 ... | disp 0x391ed"""
import sys, struct; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo, hi = b.text
code = b.rd(lo, hi - lo)
mode = sys.argv[1]
for t in sys.argv[2:]:
    t = int(t, 16)
    if mode == 'call':
        out = []
        for i in range(len(code) - 5):
            if code[i] in (0xe8, 0xe9) and (lo + i + 5 + struct.unpack('<i', code[i+1:i+5])[0]) & 0xffffffff == t:
                out.append(hex(lo + i))
        print(hex(t), out)
    else:
        needle = struct.pack('<I', t); out = []; j = code.find(needle)
        while j >= 0: out.append(hex(lo + j)); j = code.find(needle, j + 1)
        print(hex(t), out)
