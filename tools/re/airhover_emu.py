"""Unicorn harness: TotalA.exe's AirToGroundHover loop (observe only).

Per game tick, as the unit update runs it (0x48af98 -> 0x48afaa): the mission
dispatcher's event test (modelled here, 0x43b7c0), the mission handler 0x413470,
then the mover's navigator update (vtable +8, 0x490690) and the VTOL mover
0x43d290; the position commit (0x43d7ff: position += velocity) is done here.
The Brawler's numbers (armbrawl: maxvelocity 6.6, acceleration 0.16, brakerate
4, turnrate 800, cruisealt 60, BankScale 1, VTOL_EMG range 370) over flat ground
at height 100. Prints the flyer's trajectory relative to its target, and RAW
lines (x, y, z 16.16 relative to the target; heading, roll, pitch words).
Usage: airhover_emu.py x y z vx vy vz heading speed [ticks] [stage]
         (16.16 raw; x, z relative to the target at 2000, 2000; from stage 2)
       airhover_emu.py dump <the 24 numbers of flyer_combat_test's DUMP line> ticks
         (HOVER_EMU_DUMP=1 flyer_combat_test <install>: World's state at tick
         200 of the jink fixture, then its RAW lines; the next RAW lines here
         match World's bit for bit)
"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import *

b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
HEAP, STACK, STOP = 0x11000000, 0x12000000, 0x14000000
u = Uc(UC_ARCH_X86, UC_MODE_32)
u.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
u.mem_write(b.base, bytes(b.img))
for a, n in ((HEAP, 0x400000), (STACK, 0x100000), (STOP, 0x1000)):
    u.mem_map(a, n)
u.mem_write(STOP, b'\xf4')
heap = [HEAP]
def alloc(n):
    a = heap[0]; heap[0] += (n + 0xff) & ~0xff
    u.mem_write(a, bytes(n)); return a
def w8(a, v): u.mem_write(a, struct.pack('<B', v & 0xff))
def w16(a, v): u.mem_write(a, struct.pack('<H', v & 0xffff))
def w32(a, v): u.mem_write(a, struct.pack('<I', v & 0xffffffff))
def r8(a): return u.mem_read(a, 1)[0]
def r16(a): return struct.unpack('<H', u.mem_read(a, 2))[0]
def r32(a): return struct.unpack('<I', u.mem_read(a, 4))[0]
def s32(a): return struct.unpack('<i', u.mem_read(a, 4))[0]

TERRAIN, SEA = 100, 20
stubs = {}
def stub(addr, pop, fn=lambda args: 0):
    stubs[addr] = (pop, fn)
def hook(uc, addr, size, _):
    if addr in stubs:
        pop, fn = stubs[addr]
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = r32(esp)
        args = [r32(esp + 4 + 4 * i) for i in range(4)]
        uc.reg_write(UC_X86_REG_EAX, fn(args) & 0xffffffff)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        uc.reg_write(UC_X86_REG_EIP, ret)
u.hook_add(UC_HOOK_CODE, hook)
def bad(uc, access, addr, size, value, _):
    print('unmapped', hex(access), hex(addr), 'at', hex(uc.reg_read(UC_X86_REG_EIP))); return False
u.hook_add(UC_HOOK_MEM_UNMAPPED, bad)

# _hypot (CRT, 0x4fb440): sqrt(x*x + y*y) in the FPU, the CRT's error paths left out.
u.mem_write(0x4fb440, bytes.fromhex('dd442404d8c8dd44240cd8c8dec1d9fac3'))
stub(0x4898b0, 4)                          # slot mode
stub(0x48a060, 0xc)                        # slot target
stub(0x49abb0, 0xc, lambda a: 1)           # in reach
stub(0x485070, 4, lambda a: TERRAIN)       # terrain height under a point
stub(0x4b4f10, 0, lambda a: alloc(a[0]))   # operator new
stub(0x4b4f20, 0)                          # operator delete
stub(0x4b6c30, 0, lambda a: 0)             # rand

def call(fn, args, ecx=0):
    esp = STACK + 0xf0000
    for a in reversed(args):
        esp -= 4; w32(esp, a)
    esp -= 4; w32(esp, STOP)
    u.reg_write(UC_X86_REG_ESP, esp); u.reg_write(UC_X86_REG_ECX, ecx)
    u.reg_write(UC_X86_REG_FPCW, 0x27f)
    u.emu_start(fn, STOP, count=2000000)
    return u.reg_read(UC_X86_REG_EAX)

# Game, sectors, types.
G = alloc(0x40000); w32(0x511de8, G)
w8(G + 0x1427f, SEA); OFF = alloc(0x10); w32(G + 0x142b7, OFF)
w32(G + 0x14263, 8155); w32(G + 0x1422b, 4096); w32(G + 0x1422f, 4096)
S = alloc(0x10); w8(S + 1, TERRAIN)
T = alloc(0x300)
w32(T + 0x192, 432537); w32(T + 0x19a, 262144); w32(T + 0x19e, 10485); w16(T + 0x1ba, 800)
w16(T + 0x21c, 60); w32(T + 0x241, 0x800); w32(T + 0x1fa, 920)
w32(T + 0x1a2, 65536); w32(T + 0x1a6, 0)
W = alloc(0x100); w32(W + 0xdc, 370)
TT = alloc(0x300)
def mkunit(t, x, y, z, h):
    o = alloc(0x200)
    w32(o + 0x92, t); w32(o + 0x82, S); w16(o + 0xa6, 1); w16(o + 0x108, 920)
    w32(o + 0x6a, x); w32(o + 0x6e, y); w32(o + 0x72, z); w16(o + 0x66, h)
    return o
TX, TZ = 2000 << 16, 2000 << 16
tgt = mkunit(TT, TX, TERRAIN << 16, TZ, 0)

if sys.argv[1] == 'dump':
    d = [int(v) for v in sys.argv[2:26]]; ticks = int(sys.argv[26])
    w32(T + 0x192, d[23])   # World's unit speed (see the report: TA has no per-unit speed)
    x, y, z, vx, vy, vz, hd, spd = d[:8]; stage = 3
else:
    a = [int(v, 0) for v in sys.argv[1:]]; d = None
    x, y, z, vx, vy, vz, hd, spd = a[:8]
    ticks = a[8] if len(a) > 8 else 600
    stage = a[9] if len(a) > 9 else 2
U = mkunit(T, TX + x, y, TZ + z, hd); w32(U + 0x10, W)
M = alloc(0x40); w32(U, M); w8(M + 0x2e, 2)
w32(M + 8, vx); w32(M + 0xc, vy); w32(M + 0x10, vz); w32(M + 0x20, spd)
N = alloc(0x40); call(0x490940, [U], ecx=N); w32(M, N)
MS = alloc(0x60)
w8(MS + 4, 10); w8(MS + 5, stage); w32(MS + 0xa, 0xffffffff); w32(MS + 0xe, U); w32(MS + 0x16, tgt)
if d:
    for i in range(3): w32(M + 0x14 + 4 * i, d[8 + i])
    w32(N + 0xc, TX + d[11]); w32(N + 0x10, d[12]); w32(N + 0x14, TZ + d[13])
    w32(N + 0x18, d[14]); w32(N + 0x1c, d[15]); w32(N + 0x20, d[16]); w16(N + 0x24, d[22])
    pt = alloc(12); w32(pt, TX + d[17]); w32(pt + 4, d[18]); w32(pt + 8, TZ + d[19])
    c = call(0x44e330, [MS, tgt, pt], ecx=alloc(0x36))
    call(0x44e730, [0x10], ecx=c); call(0x44e6c0, [60], ecx=c); call(0x4388d0, [c], ecx=MS)
    w32(MS + 6, 0x100e8); w32(MS + 0x36, d[20]); w32(MS + 0x3a, d[21])

for tick in range(ticks):
    w32(G + 0x38a47, tick)
    # 0x43b7c0: events = (unit word | pending) & mask.
    mask, pend = r32(MS + 6), r32(MS + 0x4e)
    ev = pend & mask
    ran = ''
    if not mask or ev:
        w32(MS + 0x4e, pend & ~ev); w32(MS + 6, 0)
        rc = call(0x413470, [U, MS, ev])
        if rc == 1: w8(MS + 5, r8(MS + 5) + 1)
        elif rc != 2: print('rc', rc); break
        ran = f' ran rc={rc} side={r32(MS+0x36)}'
    nv = r32(M)
    call(r32(r32(nv) + 8), [], ecx=nv)          # navigator update
    call(0x43d290, [U], ecx=M)                   # VTOL mover
    for off, voff in ((0x6a, 8), (0x6e, 0xc), (0x72, 0x10)):
        w32(U + off, s32(U + off) + s32(M + voff))
    c = r32(MS + 0x52)
    gp = (s32(c + 0x26) - TX, s32(c + 0x2e) - TZ) if c else (0, 0)
    print(f't={tick} rel=({(s32(U+0x6a)-TX)/65536:.1f},{(s32(U+0x72)-TZ)/65536:.1f}) y={s32(U+0x6e)/65536:.1f} '
          f'hd={r16(U+0x66):04x} v=({s32(M+8)/65536:.2f},{s32(M+0x10)/65536:.2f}) st={r8(MS+5)} '
          f'goal=({gp[0]/65536:.1f},{gp[1]/65536:.1f}) f={r16(c+8) if c else 0:x}{ran}')
    print(f'RAW {s32(U+0x6a)-TX} {s32(U+0x6e)} {s32(U+0x72)-TZ} {r16(U+0x66)} {r16(U+0x64)} {r16(U+0x68)}')
