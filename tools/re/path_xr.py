import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe,struct
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
lo_t,hi_t=b.text
seg=bytes(b.img[lo_t-b.base:hi_t-b.base])
import re
for i in range(len(seg)-3):
    v=struct.unpack_from('<I',seg,i)[0]
    if lo<=v<hi: print(hex(lo_t+i),hex(v))
