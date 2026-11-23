"""Unicorn harness for dynamic checks of TotalA.exe music routines (observe only).

Runs individual retail routines with stubbed callees and prints observed
behaviour. Addresses only; nothing from the binary is copied into the engine.
Usage: music_emu.py [monitor|select|fade|all]
"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import *

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
b = pe.Bin(EXE)

GAME, CDOBJ, STACK, SCR, STOP = 0x10000000, 0x11000000, 0x12000000, 0x13000000, 0x14000000


class Emu:
    def __init__(self):
        u = self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        size = (len(b.img) + 0xfff) & ~0xfff
        u.mem_map(b.base, size)
        u.mem_write(b.base, bytes(b.img))
        for a, n in ((GAME, 0x80000), (CDOBJ, 0x1000), (STACK, 0x100000), (SCR, 0x10000), (STOP, 0x1000)):
            u.mem_map(a, n)
        u.mem_write(STOP, b'\xf4')  # hlt
        self.w32(0x511de8, GAME)
        self.w32(GAME + 0x10, CDOBJ)
        self.stubs = {}
        self.log = []
        self.mci_reply = {}
        u.hook_add(UC_HOOK_CODE, self._code)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._bad)
        # imports reached through the IAT: point every IAT slot at a unique fake address
        self.iat = {}
        for slot, (dll, name) in b.imports.items():
            fake = SCR + 0x8000 + len(self.iat) * 4
            self.iat[fake] = name
            self.w32(slot, fake)

    def _bad(self, u, access, addr, size, value, _):
        print('unmapped access', hex(addr), 'at eip', hex(u.reg_read(UC_X86_REG_EIP)))
        return False

    def w32(self, a, v): self.u.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def r32(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def s32(self, a): return struct.unpack('<i', self.u.mem_read(a, 4))[0]
    def w8(self, a, v): self.u.mem_write(a, bytes([v & 0xff]))
    def w16(self, a, v): self.u.mem_write(a, struct.pack('<H', v & 0xffff))
    def cstr(self, a):
        s = bytes(self.u.mem_read(a, 256)); return s[:s.index(0)].decode('latin1')
    def arg(self, i): return self.r32(self.u.reg_read(UC_X86_REG_ESP) + 4 + 4 * i)

    def ret(self, eax, nargs):
        u = self.u; esp = u.reg_read(UC_X86_REG_ESP)
        ra = self.r32(esp)
        u.reg_write(UC_X86_REG_EAX, eax & 0xffffffff)
        u.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * nargs)
        u.reg_write(UC_X86_REG_EIP, ra)

    def _code(self, u, addr, size, _):
        if addr in self.stubs:
            self.stubs[addr](self)
        elif addr in self.iat:
            name = self.iat[addr]
            if name == 'mciSendStringA':
                cmd = self.cstr(self.arg(0)); buf = self.arg(1)
                self.log.append(('mci', cmd))
                rep = self.mci_reply.get(cmd)
                if rep is not None and buf:
                    u.mem_write(buf, rep.encode() + b'\0')
                self.ret(0, 4)
            elif name == 'auxSetVolume':
                self.log.append(('auxSetVolume', self.arg(0), hex(self.arg(1)))); self.ret(0, 2)
            else:
                raise RuntimeError('unstubbed import ' + str(name))

    def call(self, fn, args=(), ecx=0):
        u = self.u
        esp = STACK + 0xf0000
        for a in reversed(args):
            esp -= 4; self.w32(esp, a)
        esp -= 4; self.w32(esp, STOP)
        u.reg_write(UC_X86_REG_ESP, esp)
        u.reg_write(UC_X86_REG_ECX, ecx)
        u.emu_start(fn, STOP, count=2_000_000)
        return u.reg_read(UC_X86_REG_EAX)


# ---------------------------------------------------------------- battle monitor
def monitor():
    """0x494e70: sample once per >30 time units, decide category 0/1."""
    print('== battle monitor 0x494e70')
    BUCKETS, IDX, CNT, LAST, LASTT = 0x51e710, 0x51f2dc, 0x51f2fc, 0x5091d0, 0x51f2f8

    def run(units, damage_per_sample, start_cat, samples, label, reset_last=True):
        e = Emu()
        t = [1000]
        e.stubs[0x4b6340] = lambda e: e.ret(t[0], 0)            # scaled tick clock
        e.stubs[0x46c8b0] = lambda e: e.ret(0, 0)
        sets = []
        def setcat(e):
            sets.append((n[0], e.arg(0))); e.w32(CDOBJ + 0x278, e.arg(0)); e.ret(0, 1)
        e.stubs[0x4ce690] = setcat
        e.w8(GAME + 0x2a44, 4)          # in-game flag
        e.w8(GAME + 0x2a42, 0)          # local player 0
        e.w16(GAME + 0x1ca7, units)     # player0 "Total Units"
        e.w32(CDOBJ + 0x278, start_cat)
        for i in range(30): e.w32(BUCKETS + 4 * i, 0)
        e.w32(IDX, 0); e.w32(CNT, 0); e.w32(LASTT, 0)
        if reset_last is not True: e.w32(LAST, reset_last)
        n = [0]
        for s in range(samples):
            dmg = damage_per_sample(s)
            e.call(0x494ff0, [dmg])       # accumulate into current bucket (retail adder)
            t[0] += 31
            n[0] = s + 1
            e.call(0x494e70)
        print(f'  {label}: setCategory calls (sample#, cat) = {sets}')
        return sets

    run(31, lambda s: 7, 0, 20, 'units=31, 7 pts/s, start build')
    run(30, lambda s: 7, 0, 20, 'units=30, 7 pts/s, start build')
    run(31, lambda s: 51 if s == 0 else 0, 0, 20, 'units=31, single 51-pt burst at s0')
    run(31, lambda s: 50 if s == 0 else 0, 0, 20, 'units=31, single 50-pt burst at s0')
    run(31, lambda s: 31 if s == 5 else 0, 0, 20, 'units=31, single 31-pt burst at s5')
    run(31, lambda s: 31 if s == 13 else 0, 0, 20, 'units=31, single 31-pt burst at s13')
    run(31, lambda s: 0, 1, 80, 'start battle, no damage')
    run(31, lambda s: 1 if s % 4 == 0 else 0, 1, 120, 'start battle, 1 pt every 4 s')
    run(31, lambda s: 9 if s == 60 else 0, 1, 100, 'start battle, 9 pts at s60')
    run(31, lambda s: 7, 0, 20, 'units=31, 7 pts/s, start build, stale last=1', reset_last=1)


# ---------------------------------------------------------------- track selection
def select():
    """0x4cdb40 in mode 4 (Custom/battle status) with the default 7 battle + 9 build table."""
    print('== track selection 0x4cdb40 / play 0x4ceb60')
    for mode, cat, rands, cur in ((4, 0, [0, 1, 2, 5, 15], 1), (4, 1, [0, 1, 2, 6, 15], 9), (4, 1, [3], 5),
                                  (1, 0, [0], 15), (1, 0, [0], 16), (2, 0, [0, 5, 31], 3), (3, 0, [0], 3),
                                  (4, 2, [0], 3), (4, 4, [0], 3)):
        for r in rands:
            e = Emu()
            e.stubs[0x4e4870] = lambda e, r=r: e.ret(r, 0)        # rand()
            e.stubs[0x4b6220] = lambda e: e.ret(SCR, 0)            # returns struct; +0x40 = notify hwnd
            e.stubs[0x4b64d0] = lambda e: e.ret(0, 1)              # kill timer
            e.mci_reply['status cdaudio mode'] = 'stopped'
            e.w32(CDOBJ + 0x200, 16); e.w32(CDOBJ + 0x280, 1)
            e.w32(CDOBJ + 0x1fc, mode); e.w32(CDOBJ + 0x278, cat)
            e.w32(CDOBJ + 0x27c, 1); e.w32(CDOBJ + 0x208, cur); e.w32(CDOBJ + 0x204, 3)
            e.w32(CDOBJ + 0x20c, 1); e.w32(CDOBJ + 0x20, 0x8000)
            table = [1] * 7 + [0] * 9
            for i, c in enumerate(table): e.w8(CDOBJ + 0x215 + i, c)
            e.call(0x4cdb40, ecx=CDOBJ)
            plays = [x[1] for x in e.log if x[0] == 'mci' and ('play' in x[1] or 'stop' in x[1])]
            print(f'  mode={mode} cat={cat} cur={cur} rand={r}: {plays} -> cur={e.s32(CDOBJ + 0x208)}')


# ---------------------------------------------------------------- category change / fade
def fade():
    print('== category change 0x4ce690 / fade step 0x4ce5e0')
    e = Emu()
    timers = []
    def addtimer(e):
        timers.append((e.arg(0), e.arg(1), hex(e.arg(2)))); e.ret(len(timers), 3)
    e.stubs[0x4b63f0] = addtimer
    e.stubs[0x4b64d0] = lambda e: e.ret(0, 1)
    called = []
    e.stubs[0x4cdb40] = lambda e: (called.append('select'), e.ret(0, 0))
    e.w32(0x50b540, 0xffffffff); e.w32(0x50b544, 0xffffffff)
    e.w32(CDOBJ + 0x1fc, 4); e.w32(CDOBJ + 0x278, 0); e.w32(CDOBJ + 0x20, 0x8000); e.w32(CDOBJ + 0x14, 7)
    e.w32(CDOBJ + 0x208, 9)
    e.call(0x4ce690, [1], ecx=CDOBJ)
    print('  timers added (interval, arg, callback):', timers, ' fade step [0x284] =', e.s32(CDOBJ + 0x284))
    e.w32(0x51ff14, CDOBJ); e.w32(0x50b544, 1)
    steps = 0
    while not called and steps < 40:
        e.call(0x4ce5e0, [0]); steps += 1
    vols = [int(x[2], 16) & 0xffff for x in e.log if x[0] == 'auxSetVolume']
    print(f'  fade callbacks until track select: {steps}; volumes: {vols[:3]}..{vols[-3:]}; select called: {called}')
    print('  saved track for old category at 0x51ff20[0] =', e.r32(0x51ff20))
    # battle -> build: fade then 120-unit delay timer
    e = Emu(); timers = []
    e.stubs[0x4b63f0] = addtimer
    e.stubs[0x4b64d0] = lambda e: e.ret(0, 1)
    called = []
    e.stubs[0x4cdb40] = lambda e: (called.append('select'), e.ret(0, 0))
    e.w32(0x50b540, 0xffffffff); e.w32(0x50b544, 0xffffffff)
    e.w32(CDOBJ + 0x1fc, 4); e.w32(CDOBJ + 0x278, 1); e.w32(CDOBJ + 0x20, 0x8000); e.w32(0x51ff14, CDOBJ)
    e.call(0x4ce690, [0], ecx=CDOBJ); e.w32(0x50b544, 1)
    steps = 0
    while len(timers) < 2 and steps < 40:
        e.call(0x4ce5e0, [0]); steps += 1
    print(f'  battle->build: after {steps} fade steps timers={timers}, select called={called}')
    # mode 1 (Play All): change category 0 -> 1 should not interrupt
    e = Emu(); timers = []
    e.stubs[0x4b63f0] = addtimer
    called = []
    e.stubs[0x4cdb40] = lambda e: (called.append('select'), e.ret(0, 0))
    e.w32(CDOBJ + 0x1fc, 1); e.w32(CDOBJ + 0x278, 0)
    e.call(0x4ce690, [1], ecx=CDOBJ)
    print(f'  mode1 cat0->1: timers={timers} select={called}')
    e.call(0x4ce690, [2], ecx=CDOBJ)
    print(f'  mode1 cat1->2: timers={timers} select={called}')


# ---------------------------------------------------------------- default category table
def tables():
    print('== default track tables')
    e = Emu()
    e.mci_reply = {}
    e.stubs[0x4b6b60] = lambda e: e.ret(0, 1)
    e.stubs[0x4cda00] = lambda e: e.ret(16, 0)
    e.stubs[0x4b64d0] = lambda e: e.ret(0, 1)
    e.w32(CDOBJ, 0)
    e.call(0x4ce260, ecx=CDOBJ)
    print('  after init 0x4ce260: cat[0..17] =', list(e.u.mem_read(CDOBJ + 0x214, 18)),
          ' mode', e.r32(CDOBJ + 0x1fc), ' sel', e.r32(CDOBJ + 0x204), ' cat', e.r32(CDOBJ + 0x278))


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    for name, f in (('monitor', monitor), ('select', select), ('fade', fade), ('tables', tables)):
        if what in (name, 'all'): f()
