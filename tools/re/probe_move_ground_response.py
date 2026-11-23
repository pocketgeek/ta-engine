#!/usr/bin/env python3
"""Retail Move_Ground's attack response (0x402e10..0x4032a8), emulated
(2026-11-03).

Entered mid-function at 0x402e10 (the response block, after Move_Ground's
event tests) with a hand-built frame: ebx = the Move mission, esi = the
unit, [ebp+0x10] = events 0.  Runs natively: the response itself, the
scanning slot 0x519ac0, the distance 0x535e90 and the real weapon-class
vtables (generic 0x5f3710, melee 0x5f3af8).  Hooked: the mission scan
0x4d8370 (scripted), the in-range test 0x52ad60 (scripted), damage 0x531e50
(100), the resolver 0x4de530, operator new / free, the Move's controller
reset 0x4d4d40, the mission ctor 0x4d6c40 and the insert 0x4d7750 (both
logged).  Observation only; reads the user's own retail binary; nothing is
copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP, STACK, STACK_SZ, RETURN_MAGIC
from unicorn.x86_const import (UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EBX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EAX)

p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
get8 = lambda a: uc.mem_read(a, 1)[0]
gets16 = lambda a: struct.unpack('<h', uc.mem_read(a, 2))[0]

U, D, A, AD = HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000
OWN, ENEMY = HEAP + 0x5000, HEAP + 0x5200
M = HEAP + 0x6000
WEAP, CLS = HEAP + 0x7000, HEAP + 0x7100
MASKS = HEAP + 0x7400
bump = [HEAP + 0x100000]
log = []
st = {}
def new(uc_, sp):
    bump[0] += 0x100; uc.mem_write(bump[0], bytes(0x100)); return 0, bump[0]
def resolver(uc_, sp):
    out, kind = struct.unpack('<2I', uc.mem_read(sp, 8))
    uc.mem_write(out, bytes((9 if kind == 3 else kind,))); log.append(('resolve', kind)); return 6, out
def ctor(uc_, sp):
    this = uc.reg_read(UC_X86_REG_ECX)
    a = struct.unpack('<12I', uc.mem_read(sp, 48))
    uc.mem_write(this + 4, bytes((a[0] & 0xff,)))
    log.append(('ctor', dict(kind=a[0] & 0xff, target=a[1], point=a[2], f4e=a[3], mode=a[4], leash=a[5])))
    return 12, this
def insert(uc_, sp):
    unit, m = struct.unpack('<2I', uc.mem_read(sp, 8))
    log.append(('insert', (gets16(m + 0x2e), gets16(m + 0x30)))); return 2, 0
def scan(uc_, sp):
    log.append(('scan',)); return 1, st['scan']
def inrange(uc_, sp):
    return 3, int(st['inrange'])
p.hooks.update({0x4eb9e0: new, 0x4de530: resolver, 0x4d6c40: ctor, 0x4d7750: insert,
                0x4d8370: scan, 0x52ad60: inrange, 0x531e50: lambda u, s: (1, 100),
                0x4d4d40: lambda u, s: (1, 0), 0x4d6da0: lambda u, s: (0, 0), 0x4eba00: lambda u, s: (0, 0)})
p.freeze_hooks()

STANCE = {'Hold Position': 0, 'Maneuver': 0x10000, 'Roam': 0x20000}
FIRE = {'Hold Fire': 0, 'Return Fire': 0x40000, 'Fire At Will': 0x80000}

def run(mode, stance='Roam', fire='Fire At Will', scan_hit=False, attacker=None, inr=False,
        melee=False, rng=300, leash=200, goal=(1500, 1000), cloaked=False, allied=False, selectable=True):
    for a, n in ((U, 0x138), (D, 0x300), (A, 0x138), (AD, 0x300), (OWN, 0x200), (ENEMY, 0x200),
                 (M, 0x80), (WEAP, 0x100), (CLS, 0x10), (MASKS, 0x40)):
        uc.mem_write(a, bytes(n))
    put(U + 0xb4, D); put(U + 0xb8, OWN); put8(OWN + 0xeb, 0)
    put(U + 0x130, 0x1000000 | 0x8000000 | (0x20 if selectable else 0) | STANCE[stance] | FIRE[fire] | 1)
    put(U + 0x68, 1000 << 16); put(U + 0x70, 1000 << 16)
    put16(D + 0x232, leash); put(D + 0x264, 0x20)
    for off in (0x9a, 0x8e, 0x92, 0x96): put(D + off, MASKS)
    put(CLS, 0x5f3af8 if melee else 0x5f3710); put(WEAP + 0x40, CLS); put(WEAP + 0x90, rng)
    put(U + 0xc, WEAP); put(D + 0x1aa, WEAP)
    put(A + 0xb4, AD); put(A + 0xb8, ENEMY); put8(ENEMY + 0xeb, 1); put16(A, 5)
    put(A + 0x130, 0x1000000 | 1); put8(A + 0x114, 4 if cloaked else 0)
    put8(OWN + 0xac + 1, 1 if allied else 0)
    if attacker is not None:
        put(A + 0x68, (1000 + attacker) << 16); put(A + 0x70, 1000 << 16)
        put(U + 0xf8, A)
    T = HEAP + 0x8000; uc.mem_write(T, bytes(0x138)); put(T + 0x68, 1300 << 16); put(T + 0x70, 1000 << 16)
    st.update(scan=T if scan_hit else 0, inrange=inr)
    put(M + 0x52, mode); put16(M + 0x24, goal[0]); put16(M + 0x2c, goal[1]); put8(M + 5, 2)
    # frame: [ebp] saved ebp, [ebp+4] return, [ebp+8] unit, [ebp+0xc] mission, [ebp+0x10] events
    ebp = STACK + STACK_SZ - 0x2000
    put(ebp, 0); put(ebp + 4, RETURN_MAGIC); put(ebp + 8, U); put(ebp + 0xc, M); put(ebp + 0x10, 0)
    esp = ebp - 0x34 - 12
    for r, v in ((UC_X86_REG_EBP, ebp), (UC_X86_REG_ESP, esp), (UC_X86_REG_EBX, M),
                 (UC_X86_REG_ESI, U), (UC_X86_REG_EDI, 0)):
        uc.reg_write(r, v)
    log.clear()
    uc.emu_start(0x402e10, RETURN_MAGIC, timeout=5_000_000)
    res = uc.reg_read(UC_X86_REG_EAX)
    ctors = [x[1] for x in log if x[0] == 'ctor']
    inserts = [x[1] for x in log if x[0] == 'insert']
    return dict(result=res, scanned=any(x[0] == 'scan' for x in log), attack=ctors[0] if ctors else None,
                origin=inserts[0] if inserts else None, stage=get8(M + 5),
                move_point=(gets16(M + 0x2e), gets16(M + 0x30)), target_is_attacker=bool(ctors and ctors[0]['target'] == A))

checks = 0
def check(ok, label):
    global checks
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: raise SystemExit(1)
    checks += 1

for stance in STANCE:
    r = run(-1, stance=stance, scan_hit=True)
    check(r['scanned'] and r['attack'] and r['attack']['mode'] == 1 and r['attack']['leash'] == 200
          and r['origin'] == (1000, 1000) and r['stage'] == 3 and r['move_point'] == (1000, 1000),
          f'mode -1, {stance}: scans; attack built with mode 1, leash = maneuverleashlength (200), origin = the '
          f'unit\'s position; the Move keeps that point (+0x2e/+0x30) and goes to stage 3: {r}')
r = run(1, stance='Hold Position', scan_hit=True)
check(not r['scanned'] and r['attack'] is None and r['stage'] == 1, f'mode 1, Hold Position: no scan, stage 1: {r}')
r = run(0, stance='Roam', scan_hit=True)
check(not r['scanned'] and r['attack'] is None, f'mode 0: no scan: {r}')
r = run(1, stance='Roam', scan_hit=True, goal=(5000, 1000))
check(r['attack'] and r['attack']['leash'] == 200 and r['origin'] == (5000, 1000) and r['stage'] == 0,
      f'mode 1, Roam: target 3700 px from the goal still taken; origin = the goal; leash 200; stage 0: {r}')
r = run(1, stance='Maneuver', scan_hit=True, goal=(1500, 1000))
check(r['attack'] is None, f'mode 1, Maneuver: target 200 px from the goal (>= leash 200): rejected, no rescan: {r}')
r = run(1, stance='Maneuver', scan_hit=True, goal=(1400, 1000))
check(r['attack'] is not None and r['origin'] == (1400, 1000), f'mode 1, Maneuver: 100 px from the goal: taken: {r}')
# The fallback: the stored attacker +0xf8.
r = run(-1, attacker=250, inr=True)
check(r['target_is_attacker'], f'fallback: attacker in reach (52ad60) taken: {r["attack"]}')
# reach = 9/10 x leash (+ the slot's range for a non-melee slot): 180 + 300 = 480
r = run(-1, attacker=479, inr=False)
check(r['target_is_attacker'], 'fallback: out of range, 479 px < 9/10 x 200 + 300: taken')
r = run(-1, attacker=480, inr=False)
check(r['attack'] is None, 'fallback: 480 px (not < 480): not taken')
r = run(-1, attacker=179, inr=False, melee=True, rng=40)
check(r['target_is_attacker'], 'fallback, melee slot: 179 px < 9/10 x 200 (no range added): taken')
r = run(-1, attacker=181, inr=False, melee=True, rng=40)
check(r['attack'] is None, 'fallback, melee slot: 181 px: not taken')
r = run(-1, attacker=250, inr=True, cloaked=True)
check(r['target_is_attacker'], 'fallback: a cloaked attacker (+0x114 & 4) is still taken (no cloak test)')
r = run(-1, attacker=250, inr=True, allied=True)
check(r['attack'] is None, 'fallback: an allied attacker is not taken')
r = run(-1, attacker=250, inr=True, fire='Hold Fire')
check(r['target_is_attacker'], 'fallback on Hold Fire: taken (no fire-state test in the fallback; 4d8370 alone gates on Fire At Will)')
r = run(1, stance='Maneuver', attacker=150, inr=True, goal=(1000, 1000))
check(r['target_is_attacker'] and r['origin'] == (1000, 1000), 'mode 1, Maneuver: the fallback attacker 150 px from the goal: taken')
r = run(1, stance='Maneuver', attacker=250, inr=True, goal=(1000, 1000))
check(r['attack'] is None, 'mode 1, Maneuver: the fallback attacker 250 px from the goal (leash 200): rejected by the same goal leash')
# 402ece: the unit itself must be selectable (+0x130 & 0x20); one a mission
# script runs (the parse cleared it) does not answer its attacker.
r = run(-1, attacker=250, inr=True, selectable=False)
check(r['attack'] is None, 'fallback: an unselectable unit (a mission script runs it) does not answer its attacker')
print(f'PASS: {checks} observations')
