"""Unicorn harness: dynamic confirmation of TotalA.exe economy routines (observe only).

Runs individual retail routines on synthetic structs and prints observed numbers.
Nothing from the binary is copied into engine source.
Usage: economy_emu.py [settle|extract|build|repair|wind|all]
"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import *

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
b = pe.Bin(EXE)
GAME, HEAP, STACK, STOP = 0x10000000, 0x11000000, 0x12000000, 0x14000000


class Emu:
    def __init__(self):
        u = self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
        u.mem_write(b.base, bytes(b.img))
        for a, n in ((GAME, 0x80000), (HEAP, 0x100000), (STACK, 0x100000), (STOP, 0x1000)):
            u.mem_map(a, n)
        u.mem_write(STOP, b'\xf4')
        self.w32(0x511de8, GAME)
        self.stubs = {}
        self.calls = []
        u.hook_add(UC_HOOK_CODE, self._code)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._bad)
        self.heap = HEAP

    def _bad(self, u, access, addr, size, value, _):
        print('  unmapped access', hex(addr), 'eip', hex(u.reg_read(UC_X86_REG_EIP)))
        return False

    def _code(self, u, addr, size, _):
        if addr in self.stubs:
            name, nargs, ret = self.stubs[addr][:3]
            pop = self.stubs[addr][3] if len(self.stubs[addr]) > 3 else nargs
            esp = u.reg_read(UC_X86_REG_ESP)
            args = [self.r32(esp + 4 + 4 * i) for i in range(nargs)]
            self.calls.append((name, args, u.reg_read(UC_X86_REG_ECX)))
            ra = self.r32(esp)
            u.reg_write(UC_X86_REG_EAX, ret(args) if callable(ret) else ret)
            u.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * pop)
            u.reg_write(UC_X86_REG_EIP, ra)

    def alloc(self, n):
        a = self.heap; self.heap += (n + 0xff) & ~0xff; return a
    def w8(self, a, v): self.u.mem_write(a, bytes([v & 0xff]))
    def w16(self, a, v): self.u.mem_write(a, struct.pack('<H', v & 0xffff))
    def w32(self, a, v): self.u.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def wf(self, a, v): self.u.mem_write(a, struct.pack('<f', v))
    def r32(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def rf(self, a): return struct.unpack('<f', self.u.mem_read(a, 4))[0]
    def rd(self, a): return struct.unpack('<d', self.u.mem_read(a, 8))[0]
    def r16(self, a): return struct.unpack('<h', self.u.mem_read(a, 2))[0]

    def call(self, fn, args, ecx=0, fpu_reset=True):
        u = self.u
        esp = STACK + 0xf0000
        for a in reversed(args):
            esp -= 4; self.w32(esp, a)
        esp -= 4; self.w32(esp, STOP)
        u.reg_write(UC_X86_REG_ESP, esp)
        u.reg_write(UC_X86_REG_ECX, ecx)
        u.emu_start(fn, STOP, count=2_000_000)
        return u.reg_read(UC_X86_REG_EAX)


def f2i(x): return struct.unpack('<I', struct.pack('<f', x))[0]


# ---------------------------------------------------------------- settle (0x401360)
def mk_type(e, **kv):
    t = e.alloc(0x260)
    off = dict(energymake=0x1c2, energyuse=0x1c6, metalmake=0x1ca, extractsmetal=0x1ce,
               windgenerator=0x1d2, tidalgenerator=0x1d6, cloakcost=0x1da, cloakcostmoving=0x1de,
               energystorage=0x1e2, metalstorage=0x1e6, bce=0x186, bcm=0x18a)
    for k, v in kv.items():
        if k == 'makesmetal': e.w8(t + 0x22d, v)
        elif k == 'buildtime': e.w32(t + 0x1ea, v)
        elif k == 'maxdamage': e.w32(t + 0x1fa, v)
        elif k == 'workertime': e.w16(t + 0x1fe, v)
        else: e.wf(t + off[k], v)
    return t


def mk_player(e, typ=1, E=0.0, M=0.0, baseE=0.0, baseM=0.0, flag149=1):
    p = e.alloc(0x200); acc = e.alloc(0x40)
    e.w32(p, 1); e.w8(p + 0x73, typ)
    e.wf(p + 0x8c, E); e.wf(p + 0x98, M)
    e.wf(p + 0xdc, baseE); e.wf(p + 0xe0, baseM); e.w8(p + 0x149, flag149)
    e.w32(p + 0xec, acc); e.w32(acc + 0x30, p)
    return p


def mk_units(e, p, specs):
    """specs: list of dicts (type, flags, state, remaining, extract, acc dict)"""
    base = e.alloc(0x118 * len(specs))
    for i, s in enumerate(specs):
        un = base + 0x118 * i
        e.w32(un + 0x92, s['type']); e.w32(un + 0xec, p)
        e.w32(un + 0x110, s.get('flags', 0x30000000)); e.w8(un + 0x10e, s.get('state', 1))
        e.wf(un + 0x104, s.get('remaining', 0.0)); e.wf(un + 0x58, s.get('extract', 0.0))
        for k, v in s.get('acc', {}).items(): e.wf(un + 0xbc + k, v)
    e.w32(p + 0x67, base); e.w32(p + 0x6b, base + 0x118 * (len(specs) - 1))
    return [base + 0x118 * i for i in range(len(specs))]


def dump_player(e, p, tag):
    print(f'  [{tag}] storedE={e.rf(p+0x8c):.4f} storedM={e.rf(p+0x98):.4f} '
          f'Einc={e.rf(p+0x90):.4f} Ereq={e.rf(p+0x94):.4f} Minc={e.rf(p+0x9c):.4f} Mreq={e.rf(p+0xa0):.4f} '
          f'capE={e.rf(p+0xa4):.1f} capM={e.rf(p+0xa8):.1f} wasteE={e.rd(p+0xcc):.3f} wasteM={e.rd(p+0xd4):.3f}')


def dump_acc(e, a, tag):
    v = [e.rf(a + o) for o in range(0, 0x30, 4)]
    print(f'  [{tag}] Eprod={v[0]:.3f} Ereq={v[1]:.3f} Enew={v[2]:.3f} Edebt={v[3]:.4f} lastEprod={v[4]:.3f} lastEreq={v[5]:.3f}'
          f' | Mprod={v[6]:.3f} Mreq={v[7]:.3f} Mnew={v[8]:.3f} Mdebt={v[9]:.4f} lastM={v[10]:.3f},{v[11]:.3f}')


def settle():
    print('== 0x401360 settle (once per 30 ticks per player)')
    e = Emu(); e.stubs[0x48b090] = ('cloak_set', 2, 0)
    com = mk_type(e, energymake=25, metalmake=1)
    builder = mk_type(e)
    p = mk_player(e, E=500, M=500, baseE=1000, baseM=1000)
    us = mk_units(e, p, [dict(type=com), dict(type=builder, acc={4: 300, 8: 300, 0x1c: 50, 0x20: 50})])
    e.call(0x401360, [p]); dump_player(e, p, 'T1 ok request'); dump_acc(e, us[1]+0xbc, 'T1 builder')
    # shortage
    e.wf(us[1] + 0xc0, 2000); e.wf(us[1] + 0xc4, 2000)
    e.call(0x401360, [p]); dump_player(e, p, 'T2 short'); dump_acc(e, us[1]+0xbc, 'T2 builder')
    e.call(0x401360, [p]); dump_player(e, p, 'T3 debt-only'); dump_acc(e, us[1]+0xbc, 'T3 builder')
    # second debtor + new request: priority of debt over new requests
    e.wf(p + 0x8c, 600)
    e.wf(us[0] + 0xc0, 400); e.wf(us[0] + 0xc4, 400)  # commander new request 400
    e.call(0x401360, [p]); dump_player(e, p, 'T3b debt+new'); dump_acc(e, us[1]+0xbc, 'T3b builder'); dump_acc(e, us[0]+0xbc, 'T3b com')
    # overflow
    e.wf(us[1] + 0xc8, 0); e.wf(us[0] + 0xc8, 0)
    e.wf(p + 0x8c, 990); e.wf(p + 0x98, 999.5)
    e.call(0x401360, [p]); dump_player(e, p, 'T5 overflow')
    # AI multipliers
    for diff in (0, 1, 2):
        e2 = Emu(); e2.stubs[0x48b090] = ('cloak_set', 2, 0)
        t = mk_type(e2, energymake=100, metalmake=10)
        p2 = mk_player(e2, typ=2, baseE=10000, baseM=10000)
        mk_units(e2, p2, [dict(type=t)])
        e2.w32(GAME + 0x37eee, diff)
        e2.call(0x401360, [p2]); print(f'  [T4 AI diff={diff}] E={e2.rf(p2+0x8c):.2f} M={e2.rf(p2+0x98):.2f}')
    # producers: wind, tidal, solar(-eu), mex, maker, unfinished unit, storage
    e = Emu(); e.stubs[0x48b090] = ('cloak_set', 2, 0)
    e.wf(GAME + 0x37ede, 0.3); e.wf(GAME + 0x14267, 18.0)
    wind = mk_type(e, windgenerator=30); tide = mk_type(e, tidalgenerator=1)
    solar = mk_type(e, energyuse=-20); mex = mk_type(e, energyuse=3, extractsmetal=0.001)
    maker = mk_type(e, energyuse=60, makesmetal=1); stor = mk_type(e, energystorage=3000, metalstorage=1000, energymake=7)
    p = mk_player(e, E=0, M=0, baseE=0, baseM=0, flag149=0)
    us = mk_units(e, p, [dict(type=wind), dict(type=tide), dict(type=solar), dict(type=mex, extract=1.75),
                         dict(type=maker), dict(type=stor), dict(type=stor, remaining=0.5),
                         dict(type=solar, state=0), dict(type=mex, extract=2.0, acc={0xc: 5.0})])
    e.call(0x401360, [p]); dump_player(e, p, 'T6 producers')
    for i, un in enumerate(us): dump_acc(e, un + 0xbc, f'T6 u{i}')
    # unfinished / not-0x20000000 unit consuming energyuse while active
    e = Emu(); e.stubs[0x48b090] = ('cloak_set', 2, 0)
    t = mk_type(e, energyuse=10, energymake=5, extractsmetal=0.002)
    p = mk_player(e, E=100, M=0, baseE=1000, baseM=1000)
    us = mk_units(e, p, [dict(type=t, flags=0x10000000, state=1), dict(type=t, flags=0x10000000, state=0),
                         dict(type=t, flags=0x10000004, state=0)])
    e.call(0x401360, [p]); dump_player(e, p, 'T7 non-0x20000000 units')
    for i, un in enumerate(us): dump_acc(e, un + 0xbc, f'T7 u{i}')
    # cloak
    e = Emu(); e.stubs[0x48b090] = ('cloak_set', 2, 0)
    t = mk_type(e, cloakcost=50.7, cloakcostmoving=120.2)
    p = mk_player(e, E=100, M=0, baseE=1000, baseM=1000)
    us = mk_units(e, p, [dict(type=t, flags=0x30000800), dict(type=t, flags=0x30000804), dict(type=t, flags=0x30001800)])
    e.w32(GAME + 0x38a47, 10)
    e.call(0x401360, [p]); dump_player(e, p, 'T8 cloak')
    print('  cloak_set calls (ecx=unit, args=(bits, on)):', [(hex(c[2] - us[0]), c[1]) for c in e.calls])


# ---------------------------------------------------------------- extractor (0x437840)
def extract():
    print('== 0x437840 extractor yield at placement')
    e = Emu(); e.stubs[0x4b0a70] = ('cob_call', 8, 0)
    W, H = 16, 16
    cells = e.alloc(W * H * 13)
    e.w32(GAME + 0x14233, W); e.w32(GAME + 0x14237, H); e.w32(GAME + 0x14287, cells)
    for y in range(H):
        for x in range(W):
            e.w8(cells + (y * W + x) * 13 + 7, 0)
    for (x, y, m) in ((5, 5, 10), (6, 5, 20), (5, 6, 30), (6, 6, 40)):
        e.w8(cells + (y * W + x) * 13 + 7, m)
    t = mk_type(e, extractsmetal=0.001)
    un = e.alloc(0x118); e.w32(un + 0x92, t); e.w32(un + 0x9a, 0x1234)
    for (x0, y0, w, h) in ((5, 5, 2, 2), (4, 4, 3, 3), (0, 0, 3, 3)):
        e.w16(un + 0x76, x0); e.w16(un + 0x78, y0); e.w16(un + 0x7e, w); e.w16(un + 0x80, h)
        e.calls.clear(); e.call(0x437840, [un])
        print(f'  fp at ({x0},{y0}) {w}x{h}: unit+0x58={e.rf(un+0x58):.6f}  SetSpeed arg={[c[1][4] for c in e.calls]}')


# ---------------------------------------------------------------- build step (0x41ba60)
def build():
    print('== 0x41ba60 build step (per tick, amount = workertime/30)')
    for debt in (0.0, 5.0):
        e = Emu(); e.stubs[0x41b8d0] = ('complete', 2, 0); e.stubs[0x489bb0] = ('damage', 5, 0)
        t = mk_type(e, bce=1000, bcm=100, buildtime=1000, maxdamage=500)
        bt = mk_type(e, workertime=300)
        p = mk_player(e)
        tgt = e.alloc(0x118); bld = e.alloc(0x118)
        e.w32(tgt + 0x92, t); e.w32(tgt + 0xec, p); e.wf(tgt + 0x104, 1.0); e.w16(tgt + 0x108, 0)
        e.w32(bld + 0x92, bt); e.w32(bld + 0xec, p); e.wf(bld + 0xc8, debt)
        r = e.call(0x41ba60, [bld, tgt, f2i(10.0)])
        print(f'  debt={debt}: ret={r} remaining={e.rf(tgt+0x104):.4f} hp={e.r16(tgt+0x108)} '
              f'builder Ereq={e.rf(bld+0xc0):.3f} Enew={e.rf(bld+0xc4):.3f} Mreq={e.rf(bld+0xd8):.3f} Mnew={e.rf(bld+0xdc):.3f} flags={hex(e.r32(tgt+0x110))}')
    e = Emu(); e.stubs[0x41b8d0] = ('complete', 2, 0); e.stubs[0x489bb0] = ('damage', 5, 0)
    t = mk_type(e, bce=1000, bcm=100, buildtime=1000, maxdamage=500)
    p = mk_player(e); tgt = e.alloc(0x118); bld = e.alloc(0x118)
    e.w32(tgt + 0x92, t); e.w32(tgt + 0xec, p); e.wf(tgt + 0x104, 0.5); e.w16(tgt + 0x108, 250)
    e.w32(bld + 0x92, t); e.w32(bld + 0xec, p)
    r = e.call(0x41ba60, [bld, tgt, f2i(-10.0)])
    print(f'  negative amt=-10: ret={r} remaining={e.rf(tgt+0x104):.4f} hp={e.r16(tgt+0x108)} target Mprod={e.rf(tgt+0xd4):.3f} builder Mprod={e.rf(bld+0xd4):.3f} calls={e.calls}')


# ---------------------------------------------------------------- repair (0x41bd10)
def repair():
    print('== 0x41bd10 repair step')
    for (hp, bce, bt, amt) in ((3000, 34125, 95897, 10.0), (500, 100, 100, 10.0), (500, 1, 100000, 1.0)):
        e = Emu(); e.stubs[0x489bb0] = ('damage', 5, 0)
        t = mk_type(e, bce=bce, buildtime=bt, maxdamage=hp)
        p = mk_player(e); tgt = e.alloc(0x118); bld = e.alloc(0x118)
        e.w32(tgt + 0x92, t); e.w16(tgt + 0x108, 10); e.w32(bld + 0x92, t); e.w32(bld + 0xec, p)
        r = e.call(0x41bd10, [bld, tgt, f2i(amt)])
        print(f'  maxhp={hp} bce={bce} bt={bt} amt={amt}: ret={r} builder Ereq={e.rf(bld+0xc0):.3f} damage-calls={[(c[0], c[1][2], c[1][3]) for c in e.calls]}')


# ---------------------------------------------------------------- wind (0x490c40)
def wind():
    print('== 0x490c40 wind update')
    e = Emu()
    seq = iter([0, 16383, 32767])
    e.stubs[0x4e4870] = ('crt_rand', 0, lambda a: next(seq))
    e.stubs[0x4b6c30] = ('synced_rand', 1, lambda a: a[0] - 1)
    e.stubs[0x4b70ef] = ('fixsin', 2, 0, 0); e.stubs[0x4b7123] = ('fixcos', 2, 0, 0)
    e.w32(GAME + 0x1425b, 100); e.w32(GAME + 0x1425f, 2000); e.w32(GAME + 0x37ec8, 5000)
    for tick in (1, 1000, 5000):
        e.w32(GAME + 0x38a47, tick); e.w32(GAME + 0x37ec4, 0); e.calls.clear()
        e.call(0x490c40, [])
        print(f'  tick={tick}: next change tick={e.r32(GAME+0x37ec4)} speed={e.r32(GAME+0x37eda)} dir={e.r32(GAME+0x37ed8)&0xffff} '
              f'windf={e.rf(GAME+0x37ede):.4f} changed={e.r32(GAME+0x37ee2)} calls={[(c[0], c[1]) for c in e.calls]}')
    e.w32(GAME + 0x1425b, 6000); e.w32(GAME + 0x1425f, 6001); e.w32(GAME + 0x37ec4, 0)
    seq = iter([0]); e.stubs[0x4e4870] = ('crt_rand', 0, lambda a: 0)
    e.call(0x490c40, []); print(f'  speed 6000 -> windf={e.rf(GAME+0x37ede):.4f}')


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    for name, fn in (('settle', settle), ('extract', extract), ('build', build), ('repair', repair), ('wind', wind)):
        if what in (name, 'all'): fn()
