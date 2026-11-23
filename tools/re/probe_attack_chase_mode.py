#!/usr/bin/env python3
"""Retail Attack_Chase (0x403bb0): where an automatic chase's mode >= 100
comes from, and its stage-2 retarget (0x40404d..0x40409b), emulated
(2026-11-03).

1. The real 0x4d7de0 (the attack-issuing gate) with explicit=1 and with
   auto=1: the mode it passes to the mission ctor (hooked ctor 0x4d6c40,
   resolver 0x4de530, new/free, insert 0x4d7750).
2. Attack_Chase's stage-0 set-up block, entered mid-function at 0x404583
   (ebx = unit, esi = mission, edi = slot 0): what it makes of the mode.
3. The stage-2 retarget block, entered mid-function at 0x40404d: when it
   rescans (hooked 0x4d8370, rand 0x535cc0 scripted and logged, the target
   setter 0x5199f0 logged).
Observation only; reads the user's own retail binary; nothing is copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP, STACK, STACK_SZ, RETURN_MAGIC
from unicorn.x86_const import (UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EBX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EAX)

p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
get8 = lambda a: uc.mem_read(a, 1)[0]
U, D, T, M, MOVER, WEAP = HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000, HEAP + 0x5000, HEAP + 0x6000
bump = [HEAP + 0x100000]
log = []
st = {}
def new(uc_, sp):
    bump[0] += 0x100; return 0, bump[0]
def resolver(uc_, sp):
    out, kind = struct.unpack('<2I', uc.mem_read(sp, 8))
    uc.mem_write(out, bytes(({3: 9, 2: 14}.get(kind, 0),))); return 6, out
def ctor(uc_, sp):
    this = uc.reg_read(UC_X86_REG_ECX)
    a = struct.unpack('<12I', uc.mem_read(sp, 48))
    uc.mem_write(this + 4, bytes((a[0] & 0xff,)))
    log.append(('ctor', a[0] & 0xff, a[4], a[5])); return 12, this
def rand(uc_, sp):
    n = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    v = st['draws'].pop(0) % n if st['draws'] else 0
    log.append(('rand', n, v)); return 1, v
p.hooks.update({0x4eb9e0: new, 0x4de530: resolver, 0x4d6c40: ctor,
                0x4d7750: lambda u, s: (2, 0), 0x4d6da0: lambda u, s: (0, 0), 0x4eba00: lambda u, s: (0, 0),
                0x4d8370: lambda u, s: (log.append(('scan',)), (1, st['scan']))[1],
                0x535cc0: rand,
                0x5199f0: lambda u, s: (log.append(('retarget', struct.unpack('<I', uc.mem_read(s, 4))[0])), (1, 0))[1]})
p.freeze_hooks()
from unicorn import UC_HOOK_CODE
def _fell(uc_, addr, size, _):           # past the retarget: stop there
    log.append(('fell through',)); uc_.emu_stop()
uc.hook_add(UC_HOOK_CODE, _fell, begin=0x40409e, end=0x40409e)

checks = 0
def check(ok, label):
    global checks
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: raise SystemExit(1)
    checks += 1

STANCE = {'Hold Position': 0, 'Maneuver': 0x10000, 'Roam': 0x20000}
def unit(stance='Roam', mover=True, armed=True):
    uc.mem_write(U, bytes(0x138)); uc.mem_write(D, bytes(0x300))
    put(U + 0xb4, D); put(U + 8, MOVER if mover else 0)
    put(U + 0x130, 0x1000000 | (0x8000000 if armed else 0) | STANCE[stance] | 0x80000 | 1)
    put(U + 0x68, 500 << 16); put(U + 0x70, 600 << 16)
    put(U + 0xc, WEAP); put(D + 0x1aa, WEAP); put16(D + 0x232, 333)

# 1. The gate's mode.
for explicit, auto, want in ((1, 0, 0), (0, 1, 1)):
    unit(); log.clear()
    r, err = p.call(0x4d7de0, (U, T, explicit, 0, auto, 0)); assert err is None, err
    modes = [c[2] for c in log if c[0] == 'ctor' and c[1] == 9]
    check(modes == [want], f'4d7de0 explicit={explicit} auto={auto}: the attack mission is built with mode {modes}')

def enter(addr, ebx, esi, edi, flag=0):
    ebp = STACK + STACK_SZ - 0x2000
    put(ebp, 0); put(ebp + 4, RETURN_MAGIC); put(ebp + 8, ebx); put(ebp + 0xc, esi); put(ebp + 0x10, 0)
    put(ebp - 4, edi); put8(ebp + 0xf, flag)
    for r, v in ((UC_X86_REG_EBP, ebp), (UC_X86_REG_ESP, ebp - 0x24 - 12), (UC_X86_REG_EBX, ebx),
                 (UC_X86_REG_ESI, esi), (UC_X86_REG_EDI, edi)):
        uc.reg_write(r, v)
    uc.emu_start(addr, RETURN_MAGIC, timeout=5_000_000)
    return uc.reg_read(UC_X86_REG_EAX)

# 2. Stage-0 set-up: any non-zero mode becomes 100 for an armed mover.
for mode, mover, armed, want_mode, want_res in ((1, True, True, 100, 1), (0, True, True, 0, 1),
                                                (3, True, True, 100, 1), (1, False, True, 1, 7),
                                                (1, True, False, 1, 7)):
    unit(mover=mover, armed=armed); uc.mem_write(M, bytes(0x80)); put(M + 0x52, mode); put(M + 0x4e, 0)
    res = enter(0x404583, U, M, 0)
    check(geti(M + 0x52) == want_mode and res == want_res,
          f'Attack_Chase stage 0, mode {mode}, mover={mover}, armed={armed}: mode -> {geti(M + 0x52)}, result {res}')

# 3. The stage-2 retarget.
def retarget(mode, stance='Roam', draws=(1,), scan=0x1234, current=T):
    unit(stance=stance); uc.mem_write(M, bytes(0x80)); put(M + 0x52, mode); put(M + 0x16, current); put8(M + 5, 2)
    st.update(draws=list(draws), scan=scan); log.clear()
    res = enter(0x40404d, U, M, 0, flag=0)
    return res, list(log), get8(M + 5)
res, l, stage = retarget(100)
check(('scan',) not in l and l[:1] == [('rand', 1, 0)], f'mode 100: rand(1) is always 0, no rescan: {l}')
res, l, stage = retarget(102, draws=(1,))
check(l[:2] == [('rand', 3, 1), ('scan',)] and ('retarget', 0x1234) in l and stage == 0 and res == 4,
      f'mode 102, rand(3) = 1: rescans (4d8370), a different hit becomes the target, stage 0: {l}')
res, l, stage = retarget(102, draws=(0,))
check(('scan',) not in l, f'mode 102, rand(3) = 0: no rescan: {l}')
res, l, stage = retarget(102, draws=(1,), scan=T)
check(('scan',) in l and not any(x[0] == 'retarget' for x in l), f'mode 102: the scan returns the current target: kept: {l}')
res, l, stage = retarget(102, stance='Hold Position', draws=(1,))
check(not any(x[0] in ('rand', 'scan') for x in l), f'mode 102 on Hold Position: no draw, no rescan: {l}')
res, l, stage = retarget(2, draws=(1,))
check(not any(x[0] in ('rand', 'scan') for x in l), f'mode 2 (explicit chase): no draw, no rescan: {l}')
print(f'PASS: {checks} observations')
