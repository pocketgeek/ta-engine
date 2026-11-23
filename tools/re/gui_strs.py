import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re'); import pe, re
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
pat=sys.argv[1]
for va,s in b.strings(pat,3):
    xr=b.xrefs(va)
    print(hex(va),repr(s[:80]),[hex(x) for x in xr[:8]])
