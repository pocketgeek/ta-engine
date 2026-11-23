"""Disassemble the GOG win32.dll shim (observe only). Usage: music_win32dis.py [start_rva len]"""
import sys, pefile, capstone, re
p = pefile.PE('/home/pocket_geek/ta_install/win32.dll')
base = p.OPTIONAL_HEADER.ImageBase
img = p.get_memory_mapped_image()
imp = {}
for e in p.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports: imp[i.address] = (e.dll.decode(), (i.name or b'?').decode())
def cstr(va):
    o = va - base
    if not (0 <= o < len(img)): return None
    e = img.find(b'\0', o); s = img[o:e]
    return s.decode('latin1') if 2 <= len(s) < 80 and all(32 <= c < 127 for c in s) else None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
st = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1000
ln = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x2960
for ins in md.disasm(img[st:st+ln], base + st):
    s = f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
    for h in re.findall(r'0x([0-9a-f]{7,8})', ins.op_str):
        v = int(h, 16)
        if v in imp: s += f'   ; {imp[v][1][:50]}'
        else:
            c = cstr(v)
            if c: s += f'   ; "{c}"'
    print(s)
