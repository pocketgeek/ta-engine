"""Unicorn harness: TotalA.exe's RECLAIMUNIT arithmetic (observe only).

Runs 0x438650 (the reclaim work rate) and 0x489960 (may this unit reclaim that
one) on synthetic structs and prints what they return, next to the formulas
src/sim/retailreclaimunit.h and World::reclaimUnitAllowed use. Nothing from the
binary is copied into engine source.
Usage: reclaimunit_emu.py
"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EAX

b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
HEAP, STACK, STOP = 0x11000000, 0x12000000, 0x14000000

u = Uc(UC_ARCH_X86, UC_MODE_32)
u.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
u.mem_write(b.base, bytes(b.img))
for a, n in ((HEAP, 0x100000), (STACK, 0x100000), (STOP, 0x1000)):
    u.mem_map(a, n)
u.mem_write(STOP, b'\xf4')
heap = [HEAP]


def alloc(n):
    a = heap[0]; heap[0] += (n + 0xff) & ~0xff
    u.mem_write(a, bytes(n)); return a
def w16(a, v): u.mem_write(a, struct.pack('<H', v & 0xffff))
def w32(a, v): u.mem_write(a, struct.pack('<I', v & 0xffffffff))
def wf(a, v): u.mem_write(a, struct.pack('<f', v))


def call(fn, args, ecx=0):
    esp = STACK + 0xf0000
    for a in reversed(args):
        esp -= 4; w32(esp, a)
    esp -= 4; w32(esp, STOP)
    u.reg_write(UC_X86_REG_ESP, esp); u.reg_write(UC_X86_REG_ECX, ecx)
    u.emu_start(fn, STOP, count=100000)
    return u.reg_read(UC_X86_REG_EAX)


def unit(flags_b=0, unit_flags=0x10000000, kills=0, workertime=0, maxdamage=0, bcm=0.0):
    t = alloc(0x260); o = alloc(0x200)
    w32(t + 0x245, flags_b); w16(t + 0x1fe, workertime); w32(t + 0x1fa, maxdamage); wf(t + 0x18a, bcm)
    w32(o + 0x92, t); w32(o + 0x110, unit_flags); w16(o + 0xb8, kills)
    return o


def model_rate(wt, kills, md, bcm):
    work = (wt * ((kills + 5) // 5) * md * 15) & 0xffffffff
    cost = bcm if bcm > 10.0 else 10.0
    return max(1, int(work / (cost * 300.0)))


print('0x438650(unit, target, 15): the damage one reclaim record deals')
for wt, kills, md, bcm in ((80, 0, 500, 100.0), (300, 0, 2000, 55.0), (100, 12, 450, 5.0),
                           (60, 0, 26000, 2400.0), (1, 0, 10, 1000.0), (200, 4, 3000, 10.0),
                           (65535, 0, 1000000, 3.0)):
    r = unit(workertime=wt, kills=kills)
    t = unit(maxdamage=md, bcm=bcm)
    got = call(0x438650, [r, t, 15])
    want = model_rate(wt, kills, md, bcm)
    print(f'  wt {wt:5d} kills {kills:2d} maxdamage {md:7d} bcm {bcm:7.1f}: retail {got:7d} model {want:7d}',
          'ok' if got == want else 'MISMATCH')

print('0x489960(target) on the reclaimer: canreclamate (flagsB 0x400), target not airborne')
print('(unit +0x110 & 3 == 2) and its type not cancapture (flagsB 0x1000)')
for rb, tf, tb in ((0x400, 0x10000000, 0), (0, 0x10000000, 0), (0x400, 0x10000002, 0),
                   (0x400, 0x10000001, 0), (0x400, 0x10000000, 0x1000), (0x400, 0x10000003, 0)):
    r = unit(flags_b=rb); t = unit(flags_b=tb, unit_flags=tf)
    got = call(0x489960, [t], ecx=r)
    want = int(bool(rb & 0x400) and (tf & 3) != 2 and not (tb & 0x1000))
    print(f'  reclaimer flagsB {rb:#06x} target +0x110 {tf:#010x} flagsB {tb:#06x}: retail {got} model {want}',
          'ok' if got == want else 'MISMATCH')
