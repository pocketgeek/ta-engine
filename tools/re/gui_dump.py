"""Linear disassembly of a VA range to stdout (annotated strings)."""
import sys,re; sys.path.insert(0,'/home/pocket_geek/TA/tools/re'); import pe
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
a=int(sys.argv[1],16); e=int(sys.argv[2],16)
def ann(op):
    s=''
    for h in re.findall(r'0x([0-9a-f]{6})\b',op):
        v=int(h,16)
        if 0x4fc000<=v<0x52c000:
            try:
                c=b.cstr(v)
                if len(c)>=3 and all(32<=ord(ch)<127 for ch in c): s+=f'   ; "{c[:60]}"'
            except Exception: pass
    return s
while a<e:
    got=False
    for ins in b.md.disasm(b.rd(a,min(0x4000,e-a+16)),a):
        if ins.address>=e: a=e; break
        print(f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}{ann(ins.op_str)}')
        a=ins.address+ins.size; got=True
    if not got: a+=1
