import sys,re
L=open(sys.argv[1]).read().split('\n')
for i,l in enumerate(L):
    m=re.search(r'push   0x[0-9a-f]+   ; "(.*)"',l)
    if not m: continue
    # find getter call then following stores
    out=[]
    for j in range(i+1,min(i+14,len(L))):
        s=L[j][10:]
        if 'push   0x' in s and '; "' in s: break
        if re.search(r'(mov|fstp|or|and)\s+(byte|word|dword) ptr \[e(bp|si|di|bx) \+ 0x[0-9a-f]+\]',s) or 'call' in s:
            out.append(s.strip())
    print(L[i][:8], m.group(1), ' | '.join(out)[:200])
