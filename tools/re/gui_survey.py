"""Survey retail .GUI files: per gadget-type key sets and value ranges. Data only."""
import os, re, sys, collections
D = sys.argv[1] if len(sys.argv) > 1 else '/home/pocket_geek/TA/assets/tadata/guis'
def parse(txt):
    stack=[{}]; root=stack[0]; pending=None
    for tok in re.finditer(r'\[([^\]]*)\]|\{|\}|([^=;{}\[\]\n]+)=([^;\n]*);', txt):
        if tok.group(1) is not None: pending=tok.group(1).strip().upper()
        elif tok.group(0)=='{':
            d={}; stack[-1][pending]=d; stack.append(d)
        elif tok.group(0)=='}': stack.pop()
        else: stack[-1][tok.group(2).strip().lower()]=tok.group(3).strip()
    return root
def gadgets(path):
    r=parse(open(path,encoding='latin1').read())
    return [(k,v) for k,v in r.items() if k.startswith('GADGET')]
if __name__=='__main__':
    keys=collections.defaultdict(collections.Counter); vals=collections.defaultdict(set)
    ckeys=collections.Counter()
    for f in sorted(os.listdir(D)):
        if not f.endswith('.gui'): continue
        for k,g in gadgets(os.path.join(D,f)):
            c=g.get('COMMON',{}); t=c.get('id','?')
            for kk in c: ckeys[kk]+=1
            for kk,vv in g.items():
                if isinstance(vv,dict): keys[t]['['+kk+']']+=1; continue
                keys[t][kk]+=1; vals[(t,kk)].add(vv)
            for kk in ('attribs','commonattribs','assoc','texturenumber','fontnumber','colorf','colorb'):
                vals[(t,'c.'+kk)].add(c.get(kk))
    print('COMMON keys',dict(ckeys))
    for t in sorted(keys,key=lambda x:int(x) if x.lstrip('-').isdigit() else 99):
        print('TYPE',t, dict(keys[t]))
        for (tt,kk),s in sorted(vals.items()):
            if tt==t: print('   ',kk, sorted(map(str,s))[:25], '...' if len(s)>25 else '')
