"""Annotated disassembly of a TotalA.exe range (observe only).
Usage: music_dis.py START END   (hex VAs)"""
import sys
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
st, en = int(sys.argv[1], 16), int(sys.argv[2], 16)
n = (en - st)  # upper bound on instruction count
for line in b.dis(st, n).split('\n'):
    if not line.strip(): break
    if int(line[:8], 16) >= en: break
    print(line)
