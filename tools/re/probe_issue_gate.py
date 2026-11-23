#!/usr/bin/env python3
"""Retail's attack-issuing gate 0x4d7de0(unit, target, explicit, point, auto,
forceLeash), emulated (2026-11-03): the gate every automatic pick of a
mission goes through (docs/retail-engine.md, "Mission target scan").

Real 0x4d7de0; hooked: the resolver 0x4de530 (answers a mission type),
operator new 0x4eb9e0, the mission ctor 0x4d6c40 (logged: kind, mode +0x52,
leash +0x56), the insert 0x4d7750, dtor/free.  54 combinations (mover or
not, three stances, three fire states, explicit / auto / auto+force),
checked against the rule written in the docs.  Observation only; reads the
user's own retail binary; nothing is copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX
p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
U, T, D, MOVER = HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000
ATTACK, RETURN = 9, 14
bump = [HEAP + 0x100000]
log = []
def new(uc_, sp):
    bump[0] += 0x100; return 0, bump[0]
def resolver(uc_, sp):
    out, kind = struct.unpack('<2I', uc.mem_read(sp, 8))
    uc.mem_write(out, bytes(({3: ATTACK, 2: RETURN}.get(kind, 0),))); return 6, out
def ctor(uc_, sp):
    this = uc.reg_read(UC_X86_REG_ECX)
    a = struct.unpack('<12I', uc.mem_read(sp, 48))
    uc.mem_write(this + 4, bytes((a[0] & 0xff,)))
    log.append((a[0] & 0xff, a[4], a[5])); return 12, this
p.hooks.update({0x4eb9e0: new, 0x4de530: resolver, 0x4d6c40: ctor, 0x4d7750: lambda u, s: (2, 0),
                0x4d6da0: lambda u, s: (0, 0), 0x4eba00: lambda u, s: (0, 0)})
p.freeze_hooks()
uc.mem_write(U, bytes(0x200)); uc.mem_write(D, bytes(0x300))
put(U + 0xb4, D); put16(D + 0x232, 333)          # maneuverleashlength
put(U + 0x68, 500 << 16); put(U + 0x70, 600 << 16)
STANCE = {'Hold Position': 0, 'Maneuver': 0x10000, 'Roam': 0x20000}
FIRE = {'Hold Fire': 0, 'Return Fire': 0x40000, 'Fire At Will': 0x80000}

def expected(mover, stance, fire, explicit, auto, force):
    """(result, [(kind, mode, leash), ...]) as documented."""
    if explicit:
        return 1, [(ATTACK, 0, 0)]
    if fire == 'Hold Fire':
        return 0, []
    leashed = force or stance == 'Maneuver'
    if stance == 'Hold Position' and not force:
        return 0, []
    if leashed and mover:
        return 1, [(RETURN, 0, 0), (ATTACK, 1, 333)]
    return 1, [(ATTACK, 1, 0)]

bad = n = 0
for mover in (1, 0):
    put(U + 8, MOVER if mover else 0)
    for sn, sv in STANCE.items():
        for fn, fv in FIRE.items():
            for explicit, auto, force in ((1, 0, 0), (0, 1, 0), (0, 1, 1)):
                put(U + 0x130, 0x1000000 | sv | fv)
                log.clear()
                r, err = p.call(0x4d7de0, (U, T, explicit, 0, auto, force)); assert err is None, err
                got, want = (r, list(log)), expected(mover, sn, fn, explicit, auto, force)
                n += 1
                if got != want:
                    bad += 1
                    print(f'FAIL mover={mover} {sn:13} {fn:12} explicit={explicit} auto={auto} force={force}: '
                          f'got {got}, want {want}')
print(f'{"PASS" if not bad else "FAIL"}: {n - bad}/{n} combinations match the documented gate')
sys.exit(1 if bad else 0)
