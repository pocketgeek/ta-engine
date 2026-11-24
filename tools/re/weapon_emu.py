"""Unicorn checks for docs/retail-weapon-scripts.md (observe only).

Runs the small TotalA.exe helpers the weapon/aim code builds on:
  0x4b715a  atan2(a, b) -> BAM (65536 = 360 deg)
  0x4b70ef  r * sin(angle) (table, 16.16 result for r = 1<<16)
  0x4b7123  r * cos(angle)
  0x49e570  A - B on two int[3] vectors (the aim vector is AimFrom - target)
Nothing from the binary is copied into engine source.

Usage: weapon_emu.py
"""
import struct
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
b = pe.Bin(EXE)
mu = Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
mu.mem_write(b.base, bytes(b.img))
STK, DATA, RET = 0x10000000, 0x20000000, 0x30000000
mu.mem_map(STK, 0x100000); mu.mem_map(DATA, 0x10000); mu.mem_map(RET, 0x1000)
mu.mem_write(RET, b'\xf4')


def call(fn, args):
    sp = STK + 0x80000
    for a in reversed(args):
        sp -= 4; mu.mem_write(sp, struct.pack('<i', a))
    sp -= 4; mu.mem_write(sp, struct.pack('<I', RET))
    mu.reg_write(UC_X86_REG_ESP, sp)
    mu.emu_start(fn, RET)
    return struct.unpack('<i', struct.pack('<I', mu.reg_read(UC_X86_REG_EAX)))[0]


fails = 0
for a, c, want in [(0, 10, 0), (10, 0, 16384), (0, -10, 32768), (-10, 0, 49152), (10, 10, 8192)]:
    got = call(0x4b715a, [a << 16, c << 16]) & 0xffff
    print(f'4b715a({a},{c}) = {got}'); fails += got != want
for ang, s, c in [(0, 0, 65536), (0x4000, 65536, 0), (0x8000, 0, -65536), (0xc000, -65536, 0)]:
    gs, gc = call(0x4b70ef, [ang, 1 << 16]), call(0x4b7123, [ang, 1 << 16])
    print(f'angle {ang:04x}: sin {gs} cos {gc}'); fails += (gs, gc) != (s, c)
A, B, O = DATA, DATA + 0x20, DATA + 0x40
mu.mem_write(A, struct.pack('<3i', 100, 200, 300)); mu.mem_write(B, struct.pack('<3i', 1, 2, 3))
call(0x49e570, [A, B, O, O + 4, O + 8])
out = struct.unpack('<3i', mu.mem_read(O, 12))
print('49e570(A,B) =', out); fails += out != (99, 198, 297)
print('mismatches', fails)
