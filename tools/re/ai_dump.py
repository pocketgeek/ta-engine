import sys,re; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
out=open(sys.argv[3],'w')
a=lo
while a<hi:
    got=False
    for ins in b.md.disasm(b.rd(a,0x1000),a):
        if ins.address>=hi: a=hi;break
        s=b.dis(ins.address,1)
        out.write(s+'\n'); a=ins.address+ins.size; got=True
    if not got: a+=1
