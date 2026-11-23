"""Linear disassembly of a VA range with string/helper annotations (campaign RE).
usage: camp_dis.py START END"""
import sys, re; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
NAMES = {0x4c2f60: 'TDF_load', 0x4c3120: 'TDF_parseMem', 0x4c3410: 'TDF_enter', 0x4c46c0: 'TDF_GetInt',
         0x4c4760: 'TDF_GetDouble', 0x4c4800: 'TDF_GetFixed16', 0x4c48c0: 'TDF_GetStr', 0x4c58a0: 'TDF_GetLocStr',
         0x4f8a70: '_stricmp', 0x4e4ed0: 'atol', 0x4b6290: 'fatal/report', 0x436860: 'OTA_pickSchema'}
try:
    import json; NAMES.update({int(k, 16): v for k, v in json.load(open('/home/pocket_geek/ta-tmp/camp/names.json')).items()})
except Exception: pass
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
for ins in b.md.disasm(b.rd(lo, hi - lo), lo):
    s = f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
    for h in re.findall(r'0x([0-9a-f]{6,8})', ins.op_str):
        v = int(h, 16)
        if v in NAMES: s += f'   ; <{NAMES[v]}>'
        elif 0x4fc000 <= v < 0x52c000:
            try:
                c = b.cstr(v)
                if len(c) >= 2 and all(32 <= ord(ch) < 127 for ch in c): s += f'   ; "{c[:70]}"'
            except Exception: pass
    m = re.search(r'\[0x([0-9a-f]+)\]', ins.op_str)
    if m and int(m.group(1), 16) in b.imports: s += f'   ; {b.imports[int(m.group(1),16)]}'
    print(s)
