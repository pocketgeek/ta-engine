import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
cmd=sys.argv[1]
if cmd=='dis':
    print(b.dis(int(sys.argv[2],16), int(sys.argv[3]) if len(sys.argv)>3 else 60))
elif cmd=='str':
    for a,s in b.strings(sys.argv[2]): print(hex(a),repr(s), [hex(x) for x in b.xrefs(a)])
elif cmd=='xref':
    print([hex(x) for x in b.xrefs(int(sys.argv[2],16))])
if cmd=='calls':
    import struct
    lo,hi=b.text; seg=bytes(b.img[lo-b.base:hi-b.base])
    for t in sys.argv[2:]:
        t=int(t,16); out=[]
        for i in range(len(seg)-5):
            if seg[i] in (0xe8,0xe9) and (lo+i+5+struct.unpack_from('<i',seg,i+1)[0])&0xffffffff==t: out.append(hex(lo+i)+('j' if seg[i]==0xe9 else ''))
        print(hex(t),out)
if cmd=='range':
    lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
    import re
    for ins in b.md.disasm(b.rd(lo,hi-lo),lo):
        if ins.mnemonic=='nop': continue
        s=f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
        m=re.search(r'\[0x([0-9a-f]+)\]',ins.op_str)
        if m and int(m.group(1),16) in b.imports: s+=f'   ; {b.imports[int(m.group(1),16)]}'
        print(s)
if cmd=='sweep':
    lo=int(sys.argv[2],16); hi=int(sys.argv[3],16); a=lo
    data=b.rd(lo,hi-lo)
    while a<hi:
        got=False
        for ins in b.md.disasm(data[a-lo:], a):
            got=True
            if ins.mnemonic!='nop' and ins.mnemonic!='int3': print(f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}')
            a=ins.address+ins.size
        if a<hi: a+=1
