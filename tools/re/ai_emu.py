"""Unicorn harness: run individual TotalA.exe AI routines against synthetic
game/unitdef/player structures (observation only)."""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import *
from unicorn.x86_const import *

B = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
GAME = 0x10000000; GAMESZ = 0x80000
HEAP = 0x20000000; HEAPSZ = 0x200000
STACK = 0x30000000; STACKSZ = 0x100000
RET = 0x0FFF0000

class Emu:
    def __init__(self):
        mu = Uc(UC_ARCH_X86, UC_MODE_32)
        img = bytes(B.img)
        size = (len(img) + 0xfff) & ~0xfff
        mu.mem_map(B.base, size); mu.mem_write(B.base, img)
        mu.mem_map(GAME, GAMESZ); mu.mem_map(HEAP, HEAPSZ); mu.mem_map(STACK, STACKSZ)
        mu.mem_map(RET, 0x1000); mu.mem_write(RET, b'\xf4')
        mu.mem_write(0x511de8, struct.pack('<I', GAME))
        self.mu = mu; self.hp = HEAP
    def alloc(self, n, data=None):
        a = self.hp; self.hp += (n + 15) & ~15
        if data: self.mu.mem_write(a, data)
        return a
    def w32(self, a, v): self.mu.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def w16(self, a, v): self.mu.mem_write(a, struct.pack('<H', v & 0xffff))
    def w8(self, a, v): self.mu.mem_write(a, bytes([v & 0xff]))
    def wf(self, a, v): self.mu.mem_write(a, struct.pack('<f', v))
    def r32(self, a): return struct.unpack('<I', self.mu.mem_read(a, 4))[0]
    def rs8(self, a): return struct.unpack('<b', self.mu.mem_read(a, 1))[0]
    def r8(self, a): return self.mu.mem_read(a, 1)[0]
    def call(self, fn, args=(), ecx=0, fpu_ret=False, limit=2_000_000):
        sp = STACK + STACKSZ - 0x1000
        for a in reversed(args):
            sp -= 4; self.w32(sp, a)
        sp -= 4; self.w32(sp, RET)
        mu = self.mu
        mu.reg_write(UC_X86_REG_ESP, sp); mu.reg_write(UC_X86_REG_ECX, ecx)
        mu.emu_start(fn, RET, count=limit)
        return mu.reg_read(UC_X86_REG_EAX)
