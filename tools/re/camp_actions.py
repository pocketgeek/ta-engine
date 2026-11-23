"""List order-name strings referenced by each case of the action resolver switch at 0x43f0e0."""
import sys, re; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
tab = [b.u32(0x4401ec + 4 * i) for i in range(14)]
starts = sorted(set(tab))
for i, t in enumerate(tab):
    nxt = min([s for s in starts if s > t] + [0x4401dc])
    names = []
    for ins in b.md.disasm(b.rd(t, nxt - t), t):
        for h in re.findall(r'0x(50[0-9a-f]{4})', ins.op_str):
            try:
                c = b.cstr(int(h, 16))
                if c.isupper() and len(c) > 2: names.append(c)
            except Exception: pass
    print(i + 1, hex(t), sorted(set(names)))
