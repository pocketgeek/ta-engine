import sys,struct; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo,hi=b.text; seg=bytes(b.img[lo-b.base:hi-b.base])
for t in sys.argv[1:]:
    t=int(t,16); out=[]
    for i in range(len(seg)-5):
        if seg[i] in (0xe8,0xe9):
            if lo+i+5+struct.unpack_from('<i',seg,i+1)[0]==t: out.append(hex(lo+i)+('j' if seg[i]==0xe9 else ''))
    # also pointer refs
    out+= ['ptr@'+hex(x) for x in b.xrefs(t)]
    print(hex(t), out)
