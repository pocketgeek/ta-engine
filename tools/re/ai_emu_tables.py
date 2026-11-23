"""Emulate 0x409730 (per-type value/base/baseML/baseEL table builder) on synthetic unitdefs."""
import sys; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
from ai_emu import *

def run(defs, count=None, units_owned=0, maxunits=500, wind=(10.0, 0, 0), tidal=0.0):
    e = Emu(); N = len(defs) + 1
    ud = e.alloc(0x249 * N)
    nullw = e.alloc(0x115)  # id 0 weapon
    for i, d in enumerate(defs, 1):
        a = ud + 0x249 * i
        e.wf(a+0x186, d.get('bce', 0)); e.wf(a+0x18a, d.get('bcm', 0))
        e.wf(a+0x1c2, d.get('emake', 0)); e.wf(a+0x1c6, d.get('euse', 0))
        e.wf(a+0x1ce, d.get('extract', 0)); e.w8(a+0x22d, d.get('makesmetal', 0))
        e.wf(a+0x1d2, d.get('windgen', 0)); e.wf(a+0x1d6, d.get('tidalgen', 0))
        e.w16(a+0x204, d.get('radar', 0)); e.w16(a+0x206, d.get('sonar', 0))
        e.w16(a+0x1c0, d.get('w1c0', -1))
        f241 = (0x40 if d.get('builder') else 0) | (0x800 if d.get('canfly') else 0) | (0x1000000 if d.get('isfeature') else 0)
        f245 = (0x10 if d.get('canattack') else 0) | (0x100 if d.get('canload') else 0)
        e.w32(a+0x241, f241); e.w32(a+0x245, f245)
        for k in range(3):
            ws = d.get('weapons', [])
            if k < len(ws):
                w = e.alloc(0x115); dmg, rng = ws[k]
                e.w8(w+0x10a, 7); e.w16(w+0xd4, dmg); e.w32(w+0xdc, rng)
            else: w = nullw
            e.w32(a+0x1ee+4*k, w)
    e.w32(GAME+0x1438f, N); e.w32(GAME+0x1439b, ud)
    e.w16(GAME+0x1434f, maxunits)
    e.w32(GAME+0x37ec8, int(wind[1])); e.w32(GAME+0x1425f, int(wind[2]))
    e.wf(GAME+0x37ede, wind[0]); e.wf(GAME+0x14267, tidal)
    player = e.alloc(0x200); e.w16(player+0x144, units_owned)
    ai = e.alloc(0x110)
    e.w32(ai+0, player)
    v = e.alloc(N); e.w32(ai+0x91, v); e.w32(ai+0x95, v+N); e.w32(ai+0x99, v+N)
    t = e.alloc(3*N); e.w32(ai+0x69, t); e.w32(ai+0x6d, t+3*N); e.w32(ai+0x71, t+3*N)
    c = e.alloc(2*N); e.w32(ai+0x81, c); e.w32(ai+0x85, c+2*N); e.w32(ai+0x89, c+2*N)
    for i in range(1, N): e.w16(c+2*i, (count or {}).get(i, 0))
    e.call(0x409730, ecx=ai)
    return [(e.rs8(v+i), e.rs8(t+3*i), e.rs8(t+3*i+1), e.rs8(t+3*i+2)) for i in range(1, N)]

if __name__ == '__main__':
    tests = [
        ('empty', {}),
        ('bcm100', {'bcm': 100}), ('bcm1000', {'bcm': 1000}), ('bce1000', {'bce': 1000}),
        ('canattack', {'canattack': 1}),
        ('weapon dmg400 rng500', {'canattack': 1, 'weapons': [(400, 500)]}),
        ('2 weapons', {'canattack': 1, 'weapons': [(400, 500), (40, 100)]}),
        ('extractor', {'extract': 0.001}), ('maker', {'makesmetal': 1}),
        ('solar euse-20', {'euse': -20}), ('euse+5', {'euse': 5}),
        ('wind 20 (wind 10)', {'windgen': 20}),
        ('emake 15', {'emake': 15}), ('emake 50', {'emake': 50}),
        ('builder', {'builder': 1}), ('canfly', {'canfly': 1}),
        ('radar', {'radar': 100}), ('sonar', {'sonar': 100}),
        ('w1c0=0', {'w1c0': 0}), ('transport', {'canload': 1, 'canattack': 1}),
        ('feature', {'isfeature': 1, 'canattack': 1}),
    ]
    for cnt in (0, 1, 2):
        res = run([d for _, d in tests], count={i: cnt for i in range(1, len(tests)+1)})
        print('count=%d' % cnt)
        for (n, _), r in zip(tests, res): print('  %-22s value=%4d base=%4d ML=%4d EL=%4d' % (n, *r))
    res = run([{'canattack': 1, 'weapons': [(100, 300)]}], count={1: 2}, units_owned=300, maxunits=500)
    print('near cap', res)
    res = run([{'windgen': 20}], count={1: 2}, wind=(10.0, 10, 4))
    print('wind cur<max/2 ', res, run([{'windgen': 20}], count={1: 2}, wind=(10.0, 10, 6)))
