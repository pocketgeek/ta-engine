"""Check 0x4b6c30 Random(n) against Park-Miller (16807 mod 2^31-1)."""
import sys, random; sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
from ai_emu import *
e = Emu(); rng = random.Random(3); bad = 0
for i in range(3000):
    s = rng.choice([rng.randrange(1, 2**31-1), rng.randrange(1, 2**32), 1, 0x7ffffffe, 0x7fffffff, 0x80000000, 0])
    n = rng.choice([0, 1, 2, 3, 30, 255, 900, 65536, rng.randrange(1, 1 << 20)])
    e.w32(0x51fc88, s); r = e.call(0x4b6c30, (n,)); s2 = e.r32(0x51fc88)
    if n < 2: exp_r, exp_s = 0, s
    else:
        hi = (s * 16807) >> 31; lo = (s * 16807) & 0x7fffffff  # Schrage-free reference on uint32 seed
        t = (s * 16807) % 0x7fffffff
        exp_s = t if t > 0 else t + 0x7fffffff
        exp_r = exp_s % n
    if (r, s2) != (exp_r, exp_s):
        bad += 1
        if bad < 5: print('seed', hex(s), 'n', n, 'got', r, hex(s2), 'exp', exp_r, hex(exp_s))
print('mismatches', bad)
