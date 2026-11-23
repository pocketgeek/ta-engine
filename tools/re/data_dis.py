"""Dump annotated disassembly of a range of TotalA.exe (observe only)."""
import sys; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
a = int(sys.argv[1], 16); n = int(sys.argv[2]); stop = len(sys.argv) > 3
print(b.dis(a, n, stop))
