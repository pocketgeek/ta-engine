import sys,re
L=open(sys.argv[1]).read().split('\n')
for i,l in enumerate(L):
    m=re.search(r'push   0x[0-9a-f]+   ; "(.*)"',l)
    if not m: continue
    j=i+1
    while j<len(L) and not re.search(r'call   0x4c4[0-9a-f]{3}',L[j]): j+=1
    if j-i>8: continue
    out=[]; k=j+1
    while k<len(L) and k<j+40 and not re.search(r'call   0x4c4[0-9a-f]{3}',L[k]):
        s=L[k][10:]
        if re.search(r'(mov|fstp|or|and)\s+(byte|word|dword) ptr \[e(bp|si) \+ 0x[0-9a-f]+\], ',s) or re.search(r'^(or|and)\s+e[a-d]x, 0x',s.strip()):
            out.append(s.strip())
        k+=1
    print(L[i][:8], m.group(1), L[j][10:].strip()[-10:], ' | '.join(out)[:160])
