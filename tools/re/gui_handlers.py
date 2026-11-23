"""Map GUI-open calls to their button-handler callbacks and list the gadget names/strings each handler tests."""
import sys,re,bisect
L=open(sys.argv[1]).read().split('\n')
addr=[int(l[:8],16) for l in L if l[:8].strip()]
def idx(a): return bisect.bisect_left(addr,a)
def strs_in(a, maxn=900):
    i=idx(a); out=[]; 
    for l in L[i:i+maxn]:
        if re.search(r'\bret\b',l) and 'ret    4' in l or l.endswith('ret    8') or l.endswith('ret    0xc'):
            out.append('|RET'); 
            if len(out)>60: break
        m=re.search(r'; "(.*)"',l)
        if m: out.append(m.group(1))
        m=re.search(r'0x2bc0\], (\w+)',l)
        if m: out.append('STATE='+m.group(1))
    return out
for i,l in enumerate(L):
    m=re.match(r'([0-9a-f]{8})\s+call\s+0x(4aa8f0|4ab060)$',l)
    if not m: continue
    name=[re.search(r'; "(.*)"',x).group(1) for x in L[max(0,i-8):i] if '; "' in x][-1:] 
    hs=[]
    for x in L[i+1:i+40]:
        mm=re.search(r'mov\s+dword ptr \[e.x \+ (8|0x1c|0xc|0x20)\], 0x(4[0-9a-f]{5})',x)
        if mm: hs.append((mm.group(1),mm.group(2)))
    print('==',m.group(1),name,hs)
    for k,h in hs:
        if k=='8': print('   ',[s for s in strs_in(int(h,16)) if s!='|RET'][:70])
