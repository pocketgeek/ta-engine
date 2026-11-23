import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
pat=sys.argv[1]
for va,s in b.strings(pat):
    x=b.xrefs(va)
    print(hex(va),repr(s),[hex(a) for a in x][:8])
