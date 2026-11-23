# linear-sweep disassembly of .text to scratch, for grepping operand offsets
import sys; sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
import pe, capstone
b=pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
lo,hi=b.text
code=b.rd(lo,hi-lo)
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata=True
with open('/home/pocket_geek/ta-tmp/economy/text.asm','w') as f:
    for a,sz,mn,op in md.disasm_lite(code,lo):
        f.write(f'{a:08x}  {mn} {op}\n')
