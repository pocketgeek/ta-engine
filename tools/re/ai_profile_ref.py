"""Reference model of the retail AI-profile evaluation (0x4648e0 / 0x406c90 / 0x409dc0 /
0x409e90 / 0x409f80 / 0x40a040), written from observed behaviour for golden data.
usage: ai_profile_ref.py <profile.txt> <easy|medium|hard> [UNIT ...]"""
import sys, os, re, glob, struct
TAD = '/home/pocket_geek/TA/assets/tadata'

def f32(x): return struct.unpack('<f', struct.pack('<f', x))[0]

def load_units():
    units = {}
    for p in glob.glob(os.path.join(TAD, 'units', '*.fbi')):
        txt = open(p, 'rb').read().decode('latin1')
        kv = dict((m.group(1).lower(), m.group(2).strip()) for m in re.finditer(r'^\s*([A-Za-z_0-9]+)\s*=\s*([^;\r\n]*);', txt, re.M))
        if 'unitname' not in kv: continue
        units[kv['unitname'].upper()] = {'cats': set(c.upper() for c in kv.get('category', '').split()),
                                         'ai_weight': kv.get('ai_weight', ''), 'ai_limit': kv.get('ai_limit', '')}
    return units

class Tables:
    def __init__(self, units, diff):
        self.u = units; self.diff = diff; self.active = True
        self.w = {n: 100 for n in units}; self.wl = {n: False for n in units}
        self.l = {n: -1 for n in units};  self.ll = {n: False for n in units}
    def resolve(self, name):
        n = name.upper()
        if n in self.u: return [n], True                      # unit name -> lock (0x488d30)
        return [k for k, v in self.u.items() if n in v['cats']], False
    def line(self, s):
        s = s.split('#', 1)[0]; tok = s.split()[:20]
        if not tok: return
        cmd = tok[0].lower()
        if cmd == 'plan':                                      # 0x406c90
            self.active = any(t.lower() in ('any', self.diff) for t in tok[1:])
        elif cmd == 'weight' and self.active and len(tok) > 1:  # 0x406db0 -> 0x409dc0
            names, lock = self.resolve(tok[1])
            try: f = f32(float(tok[2])) if len(tok) > 2 else 0.0
            except ValueError: f = 0.0
            for n in names:
                if self.wl[n]: continue
                self.w[n] = max(0, min(100, int(f32(self.w[n] * f))))
                if lock: self.wl[n] = True
        elif cmd == 'limit' and self.active and len(tok) > 1:   # 0x406e40 -> 0x409e90
            names, lock = self.resolve(tok[1])
            try: v = int(tok[2]) if len(tok) > 2 else 0
            except ValueError: v = 0
            for n in names:
                if self.ll[n]: continue
                self.l[n] = v
                if lock: self.ll[n] = True

def evaluate(profile, diff):
    units = load_units(); t = Tables(units, diff)
    for s in open(profile, 'rb').read().decode('latin1').split('\n'): t.line(s)
    # 0x409f80 / 0x40a040: per unit, the FBI ai_weight string (offset +0xbe) is run as a
    # script line with the plan flag forced on; 0x40a040 re-runs +0xbe (not ai_limit).
    t.active = True
    for n, v in units.items():
        if v['ai_weight'] and not t.wl[n]: t.line(v['ai_weight'])
    for n, v in units.items():
        if v['ai_weight'] and not t.ll[n]: t.line(v['ai_weight'])
    return t

if __name__ == '__main__':
    t = evaluate(sys.argv[1], sys.argv[2].lower())
    names = [a.upper() for a in sys.argv[3:]] or sorted(t.w)
    for n in names: print('%-10s weight=%3d limit=%s' % (n, t.w[n], t.l[n] if t.l[n] >= 0 else 'n/a'))
