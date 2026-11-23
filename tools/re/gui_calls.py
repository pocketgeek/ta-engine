"""For each call to given targets in a linear disassembly, show string args just before it and handler stores after."""
import sys,re
lines=open(sys.argv[1]).read().split('\n'); targets=sys.argv[2:]
for i,l in enumerate(lines):
    m=re.match(r'([0-9a-f]{8})\s+call\s+0x([0-9a-f]+)$',l)
    if not m or m.group(2) not in targets: continue
    strs=[re.search(r'; "(.*)"',x).group(1) for x in lines[max(0,i-10):i] if '; "' in x]
    post=[x.split('  ',1)[1].strip() for x in lines[i+1:i+8] if re.search(r'mov\s+dword ptr \[eax \+ 0x(8|c|1c|20)\], 0x4',x)]
    print(m.group(1),m.group(2),strs[-4:],post)
