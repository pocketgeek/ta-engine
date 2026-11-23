"""Annotated disassembly dump with TDF-getter names (observe only).
usage: data_dump.py START_HEX END_HEX"""
import sys, re; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
NAMES = {0x4c2ea0:'TDF_ctor', 0x4c2f60:'TDF_load', 0x4c3410:'TDF_enter', 0x4c3240:'TDF_free',
 0x4c4630:'GetRaw(key)', 0x4c46c0:'GetInt(key,def)', 0x4c4760:'GetDouble(key,dbl def)',
 0x4c4800:'GetFixed16(out,key,def)', 0x4c48c0:'GetStr(dst,key,max,def)', 0x4c58a0:'GetLocStr',
 0x4f8a70:'stricmp', 0x4e4f70:'atoi', 0x4e4560:'atof', 0x4e43a0:'ftol'}
a = int(sys.argv[1], 16); end = int(sys.argv[2], 16)
for ins in b.md.disasm(b.rd(a, end - a), a):
    s = f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
    for h in re.findall(r'0x([0-9a-f]{6,8})', ins.op_str):
        v = int(h, 16)
        if v in NAMES: s += f'   ; {NAMES[v]}'
        elif 0x4fc000 <= v < 0x52c000:
            try:
                c = b.cstr(v)
                if len(c) >= 2 and all(32 <= ord(ch) < 127 for ch in c): s += f'   ; "{c[:60]}"'
            except Exception: pass
    print(s)
