"""Scan .text (capstone skipdata) for instructions whose operand text contains given substrings."""
import sys; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
b.md.skipdata = True
lo, hi = b.text
pats = sys.argv[1:]
hits = {p: [] for p in pats}
for ins in b.md.disasm(b.rd(lo, hi - lo), lo):
    for p in pats:
        if p in ins.op_str: hits[p].append(hex(ins.address))
for p, v in hits.items(): print(p, len(v), ' '.join(v[:60]))
