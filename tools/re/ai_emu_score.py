"""Emulate 0x40bb00 (build score) for synthetic economy states; compare with reference formula."""
import sys, random; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
from ai_emu import *

def ref(eS, mS, eMax, mMax, eInc, eExp, mInc, mExp, t, w, count, limit, mode=0, downloadable=False):
    if eS < 50 or mS < 25: return 0
    if mode == 1 and downloadable: return 0
    import struct
    f = lambda x: struct.unpack('<f', struct.pack('<f', x))[0]
    E = max(0.0, (min(int(eMax), 1000) - f(eS)) * 0.125); E = int(E)
    M = max(0.0, (min(int(mMax), 500) - f(mS)) * 0.25); M = int(M)
    if f(eInc) - f(eExp) < 1: E += 20
    if f(mInc) - f(mExp) < 1: M += 20
    if eInc < 50: E += 100
    elif eInc < 200: E += 10
    if mInc < 3: M += 100
    elif mInc < 5: M += 20
    M = max(0, min(M, 100)); E = max(0, min(E - M, 100)); G = max(0, 100 - M - E)
    if not (limit == -1 or count < limit): return 0
    v = (G*t[0] + M*t[1] + E*t[2]) * w
    return int(v / 10000)  # trunc toward zero

def emu(eS, mS, eMax, mMax, eInc, eExp, mInc, mExp, t, w, count, limit, mode=0, downloadable=False):
    e = Emu(); p = 0; N = 3; u = 1
    ud = e.alloc(0x249*N); e.w32(ud+0x249*u+0x241, 0x20 if downloadable else 0)
    e.w32(GAME+0x1438f, N); e.w32(GAME+0x1439b, ud)
    mi = e.alloc(0x100); e.w32(mi, mode); e.w32(GAME+0x391e9, mi)
    pl = GAME + 0x1b63 + p*0x14b
    for off, v in ((0x8c, eS), (0x90, eInc), (0x94, eExp), (0x98, mS), (0x9c, mInc), (0xa0, mExp), (0xa4, eMax), (0xa8, mMax)):
        e.wf(pl+off, v)
    ai = e.alloc(0x110); e.w32(0x5119c0 + 4*p, ai)
    tt = e.alloc(3*N); e.w32(ai+0x69, tt)
    for k in range(3): e.w8(tt+3*u+k, t[k])
    wb = e.alloc(N); e.w32(ai+0xb1, wb); e.w8(wb+u, w)
    c = e.alloc(2*N); e.w32(ai+0x81, c); e.w16(c+2*u, count)
    lim = e.alloc(4*N); e.w32(ai+0xd1, lim); e.w32(lim+4*u, limit)
    r = e.call(0x40bb00, (p, u))
    return struct.unpack('<i', struct.pack('<I', r))[0]

if __name__ == '__main__':
    rng = random.Random(7); bad = 0; n = 0
    cases = [(1000, 500, 1000, 500, 300, 100, 10, 2, (50, 0, 0), 100, 0, -1),
             (40, 500, 1000, 500, 300, 100, 10, 2, (50, 0, 0), 100, 0, -1),
             (1000, 500, 1000, 500, 300, 100, 10, 2, (50, 0, 0), 100, 3, 3),
             (1000, 500, 1000, 500, 300, 100, 10, 2, (50, 0, 0), 100, 0, -1, 1, True)]
    for _ in range(400):
        cases.append((rng.uniform(0, 3000), rng.uniform(0, 1500), rng.choice([1000, 1500, 6000, 600]), rng.choice([500, 1000, 200]),
                      rng.uniform(0, 400), rng.uniform(0, 400), rng.uniform(0, 12), rng.uniform(0, 12),
                      (rng.randint(0, 100), rng.randint(0, 100), rng.randint(0, 100)), rng.randint(0, 100), rng.randint(0, 5), rng.choice([-1, 2, 4])))
    for cs in cases:
        a = emu(*cs); b = ref(*cs); n += 1
        if a != b:
            bad += 1
            if bad < 8: print('MISMATCH', cs, a, b)
    print('cases', n, 'mismatches', bad)
    for cs in cases[:4]: print(cs, '->', emu(*cs))
