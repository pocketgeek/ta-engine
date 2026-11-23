import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re'); import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
a=int(sys.argv[1],16); e=int(sys.argv[2],16)
out=[]
while a<e:
    t=b.dis(a,200); ls=t.split('\n')
    for l in ls:
        if int(l[:8],16)>=e: break
        out.append(l)
    a=int(ls[-1][:8],16)+1 if len(ls)<2 else int(ls[-1][:8],16)
    if int(out[-1][:8],16)>=e-16: break
    a=int(out[-1][:8],16); out.pop()
print('\n'.join(out))
