"""Unicorn harness: TotalA.exe's nanolathe spray object (observe only).

0x4720d0 / 0x472200 allocate a 0x4c-byte object (vtable 0x4fd5b8) and call its
init 0x473b50(srcBox, dstBox, 1); this runs that init (which emits once through
0x473d50) and then the per-tick update 0x472eb0 for a few ticks, printing every
particle record (pos, dst, vel, word 0x24, colour, expiry tick). Boxes are
min/max triples in 16.16. Nothing from the binary is copied into the engine.
Usage: nano_emu.py
"""
import sys, struct
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EAX, UC_X86_REG_EIP

b = pe.Bin('/home/pocket_geek/ta_install/TotalA.exe')
HEAP, STACK, STOP, GAME = 0x11000000, 0x12000000, 0x14000000, 0x15000000

u = Uc(UC_ARCH_X86, UC_MODE_32)
u.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
u.mem_write(b.base, bytes(b.img))
for a, n in ((HEAP, 0x400000), (STACK, 0x100000), (STOP, 0x1000), (GAME, 0x100000)):
    u.mem_map(a, n)
u.mem_write(STOP, b'\xf4')
heap = [HEAP]
def w32(a, v): u.mem_write(a, struct.pack('<I', v & 0xffffffff))
def r32(a): return struct.unpack('<I', u.mem_read(a, 4))[0]
def s32(a): return struct.unpack('<i', u.mem_read(a, 4))[0]
def alloc(n):
    a = heap[0]; heap[0] += (n + 0xff) & ~0xff
    u.mem_write(a, bytes(n)); return a

w32(0x511de8, GAME)          # the game struct; tick at +0x38a47
TICK = GAME + 0x38a47
seed = [1]
def crt_rand():               # MSVC rand(): what 0x4e4870 computes per thread
    seed[0] = (seed[0] * 0x343fd + 0x269ec3) & 0xffffffff
    return (seed[0] >> 16) & 0x7fff

def ret_with(eax, nargs_cdecl=0):
    esp = u.reg_read(UC_X86_REG_ESP)
    retaddr = r32(esp)
    u.reg_write(UC_X86_REG_EAX, eax)
    u.reg_write(UC_X86_REG_ESP, esp + 4)
    u.reg_write(UC_X86_REG_EIP, retaddr)

def hook(uc, addr, size, _):
    if addr == 0x4b4f10:      # operator new (cdecl)
        n = r32(u.reg_read(UC_X86_REG_ESP) + 4); ret_with(alloc(max(n, 4)))
    elif addr == 0x4b4f20:    # operator delete
        ret_with(0)
    elif addr == 0x4e4870:    # rand
        ret_with(crt_rand())
u.hook_add(UC_HOOK_CODE, hook, begin=0x4b4f10, end=0x4b4f20)
u.hook_add(UC_HOOK_CODE, hook, begin=0x4e4870, end=0x4e4870)

def call(fn, args, ecx=0):
    esp = STACK + 0xf0000
    for a in reversed(args):
        esp -= 4; w32(esp, a)
    esp -= 4; w32(esp, STOP)
    u.reg_write(UC_X86_REG_ESP, esp); u.reg_write(UC_X86_REG_ECX, ecx)
    u.emu_start(fn, STOP, count=2000000)
    return u.reg_read(UC_X86_REG_EAX)

def box(x0, y0, z0, x1, y1, z1):
    a = alloc(24)
    for i, v in enumerate((x0, y0, z0, x1, y1, z1)): w32(a + 4 * i, v << 16)
    return a

def particles(obj):
    beg, end = r32(obj + 0x10), r32(obj + 0x14)
    out = []
    for p in range(beg, end, 0x30):
        f = [s32(p + 4 * i) for i in range(12)]
        out.append(f)
    return out

def show(obj, tick):
    print(f'-- tick {tick}: {len(particles(obj))} live')
    for f in particles(obj):
        print('  pos=(%.2f %.2f %.2f) dst=(%.2f %.2f %.2f) vel=(%.3f %.3f %.3f) w24=%#x colour=%#x exp=%d'
              % tuple([v / 65536 for v in f[0:9]] + [f[9], f[10], f[11]]))

def run(src, dst, label):
    print('=====', label)
    w32(TICK, 1000)
    obj = alloc(0x4c); w32(obj, 0x4fd5b8)
    call(0x473b50, [src, dst, 1], obj)
    print('  boxes after init: src', [s32(obj + 0x1c + 4 * i) / 65536 for i in range(6)],
          'dst', [s32(obj + 0x34 + 4 * i) / 65536 for i in range(6)],
          'end', r32(obj + 4), 'last', r32(obj + 8))
    show(obj, 1000)
    for t in range(1001, 1004):
        w32(TICK, t)
        call(0x472eb0, [], obj)   # vtable +4: move, expire, maybe emit
        show(obj, t)

# Builder spray: a point (the QueryNanoPiece position) to a 44x22x44 box.
run(box(0, 10, 0, 0, 10, 0), box(100, 0, 50, 144, 22, 94), '0x4720d0: point -> box')
# Reclaim: the box to the point (0x472200 swaps the roles).
run(box(100, 0, 50, 144, 22, 94), box(0, 10, 0, 0, 10, 0), '0x472200: box -> point')
