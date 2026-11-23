import sys,struct; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo,hi=b.text; seg=bytes(b.img[lo-b.base:hi-b.base])
A=int(sys.argv[1],16);B=int(sys.argv[2],16)
calls={}
for i in range(len(seg)-5):
    if seg[i]==0xe8:
        t=lo+i+5+struct.unpack_from('<i',seg,i+1)[0]
        if A<=t<B: calls.setdefault(t,[]).append(lo+i)
# pointer refs (vtables / registration)
for t in range(A,B):
    pass
# function starts after padding
starts=set(calls)
o=A
for a in range(A,B):
    if b.rd(a-1,1) in (b'\x90',b'\xc3',b'\xcc') and b.rd(a,1) not in (b'\x90',) and (b.rd(a-2,2) in (b'\x90\x90',) or b.rd(a-1,1)==b'\xc3'):
        pass
for t in sorted(starts):
    c=calls[t]; ext=[hex(x) for x in c if not (A<=x<B)]; inn=[hex(x) for x in c if A<=x<B]
    print(hex(t),'ext',ext[:6],'int',inn[:6], 'ptr',[hex(x) for x in b.xrefs(t)][:4])
