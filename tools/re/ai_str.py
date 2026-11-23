import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
for va,s in b.strings(sys.argv[1]):
    print(hex(va), repr(s), [hex(x) for x in b.xrefs(va)][:8])
