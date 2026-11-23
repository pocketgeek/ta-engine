import sys,struct; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
for a in sys.argv[1:]:
    va=int(a,16); r=b.rd(va,8)
    print(hex(va),'f32=',struct.unpack('<f',r[:4])[0],'f64=',struct.unpack('<d',r)[0],'u32=',hex(struct.unpack('<I',r[:4])[0]))
