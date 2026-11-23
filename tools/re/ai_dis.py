import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
print(b.dis(va,n))
