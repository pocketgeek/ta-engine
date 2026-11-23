# brute-force find E8/E9 rel32 call/jmp sites targeting given VAs (independent of linear sweep)
import sys,struct; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo,hi=b.text; code=b.rd(lo,hi-lo)
for t in sys.argv[1:]:
    t=int(t,16); out=[]
    for i in range(len(code)-5):
        if code[i] in (0xe8,0xe9):
            if (lo+i+5+struct.unpack_from('<i',code,i+1)[0])&0xffffffff==t: out.append(hex(lo+i)+('c' if code[i]==0xe8 else 'j'))
    print(hex(t),out)
