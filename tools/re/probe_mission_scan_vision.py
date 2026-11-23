#!/usr/bin/env python3
"""Retail's automatic target scans (2026-11-03): which enemies they see and how
far they reach (docs/retail-engine.md, "Mission target scan").  Runs natively, unhooked: 0x411700 (the list
rebuild) with the real LosOn test 0x4f7210 over a synthetic sight plane, then
0x4d8370 (mission scan) and 0x40f2e0 (sweep slot) with the real picker
0x412620, the real query 0x411ae0, the real in-range test 0x52ad60 and the
real weapon-class vtables (generic 0x5f3710: inRange 0x530580; melee 0x5f3af8:
inRange 0x52b980).  Hooked: vector insert/clear (0x409510 / 0x445a40), the
damage 0x531e50 (100), 0x413290 (istargetingupgrade: no), free 0x4eba00,
the slot setters 0x51a700 / 0x51a7f0 (logged).
Observation only; reads the user's own retail binary; nothing copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

GAME = HEAP + 0x40000
UNITS, USTRIDE = HEAP + 0x80000, 0x138
DEFS, DSTRIDE = HEAP + 0xa0000, 0x300
CACHE = HEAP + 0x100000
RECS = HEAP + 0x101000
WEAPONS = HEAP + 0x103000
CLASSOBJ = HEAP + 0x104000
GLOB_P, GLOB_Q, SETTINGS = HEAP + 0x107000, HEAP + 0x107100, HEAP + 0x107200
MASKS = HEAP + 0x107400
VEC = HEAP + 0x110000          # list storage, 0x1000 per list
PLANES = HEAP + 0x120000       # 0x1000 per player
W, H = 64, 64

p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
putf = lambda a, v: uc.mem_write(a, struct.pack('<f', v))
get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]

put(0x62d55c, GAME)
put(0x62d558, GLOB_P); put(GLOB_P, GLOB_Q); put(GLOB_P + 8, SETTINGS); put8(GLOB_Q + 7, 0)
put8(SETTINGS + 0x15, 1)                        # LosOn
for i in range(4):
    r = RECS + 0x100 * i
    put(r, 1); put8(r + 0xea, 1); put8(r + 0xeb, i)
    put(r + 0x88, PLANES + 0x1000 * i); put(r + 0x8c, W); put(r + 0x90, H)
    uc.mem_write(PLANES + 0x1000 * i, bytes(W * H))
for other, allied in ((0, 1), (1, 0), (2, 1), (3, 0)):
    put8(RECS + 0xac + other, allied)
# player 0's manager: record at +0, lists at +5/+0x15/+0x25/+0x35
uc.mem_write(CACHE, bytes(0x100)); put(CACHE, RECS)
for k, lst in enumerate((5, 0x15, 0x25, 0x35)):
    put(CACHE + lst + 4, VEC + 0x1000 * k); put(CACHE + lst + 8, VEC + 0x1000 * k)
    put(CACHE + lst + 0xc, VEC + 0x1000 * k + 0xff0)
put(0x62a33c, CACHE)

def vec_insert(uc_, sp):
    lst = uc.reg_read(UC_X86_REG_ECX)
    _, _, value = struct.unpack('<3I', uc.mem_read(sp, 12))
    end = get(lst + 8)
    if get(lst + 4) == 0:
        global bump
        bump += 0x1000; put(lst + 4, bump); put(lst + 0xc, bump + 0xff0); end = bump
    put(end, get(value)); put(lst + 8, end + 4)
    return 3, 0
bump = HEAP + 0x140000
def vec_clear(uc_, sp):
    lst = uc.reg_read(UC_X86_REG_ECX); put(lst + 8, get(lst + 4)); return 2, 0
slotlog = []
p.hooks.update({
    0x409510: vec_insert, 0x445a40: vec_clear,
    0x531e50: lambda uc_, sp: (1, 100),
    0x413290: lambda uc_, sp: (1, 0),
    0x4eba00: lambda uc_, sp: (0, 0),
    0x51a700: lambda uc_, sp: (slotlog.append(('set', ) + struct.unpack('<3I', uc.mem_read(sp, 12))), (3, 0))[1],
    0x51a7f0: lambda uc_, sp: (slotlog.append(('clear',) + struct.unpack('<2I', uc.mem_read(sp, 8))), (2, 0))[1],
})
p.freeze_hooks()
uc.mem_write(MASKS, bytes(0x40))

def clear_unit(i):
    u, d = UNITS + i * USTRIDE, DEFS + i * DSTRIDE
    uc.mem_write(u, bytes(USTRIDE)); uc.mem_write(d, bytes(DSTRIDE)); put(u + 0xb4, d)
    return u, d

CX = CZ = 1024
def enemy(i, dx, player=1, lit=True, cloaked=False, flyer=False):
    u, d = clear_unit(i)
    put16(u, i)
    put(u + 0x130, 0x1000000 | (2 if flyer else 1))
    put(u + 0xb8, RECS + 0x100 * player); put8(u + 0xfd, player)
    put(u + 0x68, (CX + dx) << 16); put(u + 0x70, CZ << 16)
    put(d + 0x260, 0x8000)                      # shootme
    if cloaked: put8(u + 0x114, 4)
    cell = ((CZ) >> 5) * W + ((CX + dx) >> 5)
    put8(PLANES + cell, 1 if lit else 0)       # player 0's plane only
    return u

def weapon(idx, rng, melee):
    w = WEAPONS + 0x100 * idx; uc.mem_write(w, bytes(0x100))
    obj = CLASSOBJ + 0x10 * idx
    put(obj, 0x5f3af8 if melee else 0x5f3710)   # real vtables
    put(w + 0x40, obj); put(w + 0x90, rng); put(w + 0x94, 0)
    return w

def shooter(rng, sight, melee=False, faw=True):
    u, d = clear_unit(60)
    put(u + 0x130, 0x1000000 | 0x8000000 | (0x80000 if faw else 0x40000) | 1)
    put(u + 0xb8, RECS); put8(u + 0xfd, 0)
    put(u + 0x68, CX << 16); put(u + 0x70, CZ << 16)
    put16(d + 0x226, sight)
    for off in (0x9a, 0x8e, 0x92, 0x96): put(d + off, MASKS)
    w = weapon(0, rng, melee); put(u + 0xc, w); put(d + 0x1aa, w)
    put8(u + 0x26, 4)                            # slot 0 automatic
    return u

def rebuild(units):
    put(GAME + 0x14e84, UNITS); put(GAME + 0x14e88, UNITS + 59 * USTRIDE)
    _, err = p.call(0x411700, (), ecx=CACHE); assert err is None, err
    b, e = get(CACHE + 9), get(CACHE + 0xd)
    return [get(a) for a in range(b, e, 4)]

checks = 0
def check(ok, label):
    global checks
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: raise SystemExit(1)
    checks += 1

def scan_all(sh, seeds=40, fn=0x4d8370):
    got = set()
    for s in range(seeds):
        put(0x64186c, 0x1234567 * (s + 1) | 1)
        if fn == 0x4d8370:
            r, err = p.call(0x4d8370, (sh,)); assert err is None, err
        else:
            slotlog.clear(); _, err = p.call(0x40f2e0, (sh, 0)); assert err is None, err
            r = next((x[2] for x in slotlog if x[0] == 'set'), 0)
        got.add(r)
    return got

for i in range(1, 60): clear_unit(i)
A = enemy(1, 250)                 # seen, inside a 300 range
B = enemy(2, 400)                 # seen, between range 300 and sight 500
C = enemy(3, 200, lit=False)      # inside range, on a dark cell of player 0's plane
D = enemy(4, 650)                 # seen, beyond sight
E = enemy(5, 150, cloaked=True)   # cloaked (+0x114 & 4), on a lit cell
F = enemy(6, 100, player=2)       # ally of player 0
# player 2 (an ally) sees C's cell: must not matter to player 0
put8(PLANES + 0x2000 + ((CZ >> 5) * W + ((CX + 200) >> 5)), 5)
lst = rebuild(None)
names = {A: 'A', B: 'B', C: 'C', D: 'D', E: 'E', F: 'F'}
check(sorted(names[x] for x in lst) == ['A', 'B', 'D'],
      f'411700/4f7210: list +5 = seen enemies only (dark cell, cloaked, ally excluded; an ally\'s lit plane does not count): {sorted(names[x] for x in lst)}')

sh = shooter(300, 500)
lst = rebuild(None)
got = scan_all(sh)
check(got == {A}, f'4d8370 ranged (range 300, sight 500): only A (250) ever, never B (400, seen, inside sight): {[names.get(g, g) for g in got]}')
# Only B left in range of sight: nothing.
put(A + 0x130, 0)                          # A dead
lst = rebuild(None)
got = scan_all(sh)
check(got == {0}, f'4d8370 ranged with only B (seen, 400 px, sight 500, range 300): no target: {got}')
put(A + 0x130, 0x1000000 | 1); lst = rebuild(None)
got = scan_all(sh, fn=0x40f2e0)
check(got == {A}, f'40f2e0 sweep slot (ranged): A only: {[names.get(g, g) for g in got]}')

# Melee: picker skips 52ad60, so reach is the query radius max(sight, range).
shm = shooter(40, 500, melee=True)
lst = rebuild(None)
got = scan_all(shm)
check(got <= {A, B} and B in got and A in got,
      f'4d8370 melee (sight 500): takes seen A (250) and B (400) inside sight, never C (unseen), D (650), E (cloaked): {sorted(names.get(g, g) for g in got)}')
got = scan_all(shm, fn=0x40f2e0)
check(got == {0}, f'40f2e0 sweep slot (melee, range 40): radius is the range alone, nothing within 40: {got}')
# Ranged with range > sight: mission radius is the range.
sh2 = shooter(700, 200)
lst = rebuild(None)
got = scan_all(sh2)
check(D in got and A in got, f'4d8370 ranged (range 700, sight 200): reaches seen D (650) by the range: {sorted(names.get(g, g) for g in got)}')
# Return Fire: no scan.
shr = shooter(300, 500, faw=False)
r, _ = p.call(0x4d8370, (shr,))
check(r == 0, '4d8370 on Return Fire: no scan')
r, _ = p.call(0x40f2e0, (shr, 0))
check(True, '40f2e0 on Return Fire returns without picking')
# LosOn off: 4223f0 (Mapping bit) decides instead of the plane.
put8(SETTINGS + 0x15, 0)
put(GAME + 0x19ef4, HEAP + 0x130000); uc.mem_write(HEAP + 0x130000, bytes(W * H * 2)); put8(GAME + 0x306f, 0)
cellC = (CZ >> 5) * W + ((CX + 200) >> 5)
put16(HEAP + 0x130000 + 2 * cellC, 1)       # viewer bit 0 explored at C's cell only
lst = rebuild(None)
check(sorted(names[x] for x in lst) == ['C'], f'LosOn off: list +5 = enemies on cells with the viewer\'s Mapping bit (game+0x306f) set: {sorted(names[x] for x in lst)}')
print(f'PASS: {checks} observations')
