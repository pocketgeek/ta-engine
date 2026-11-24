"""Unicorn check of TotalA.exe's ballistic aim solve 0x49a890 (observe only).

0x49a890(dx, dy, dz, weaponvelocity, minbarrelangle) is what AimX's pitch
(0x49e28e), the fire gate's re-check (0x49d614) and the range gate
(0x49ab87) call: d = P - T in 16.16, the weapon's velocity in 16.16 per tick
(weapon +0x68), the minimum barrel angle as a float in radians (weapon +0xc8)
and gravity from G+0x14263 (16.16 per tick^2; map gravity * 65536 / 900,
0x483801). Returns the elevation in BAM or 0x8000 (no solution).

Runs each case natively (x87 at 53-bit precision, as the CRT sets it,
0x4e4320) and against the engine's model (ta::taBallisticPitch in
src/sim/retailaim.h, mirrored here in Python doubles), then simulates the
shell the way 0x49cde0 launches it and 0x49bce3 moves it -- position +=
velocity, then velocity.y -= gravity, once a tick -- and reports where it
comes down relative to the aim point.

Usage: ballistic_emu.py   (prints the table the C++ test pins)
"""
import math
import struct
import pe
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
b = pe.Bin(EXE)
mu = Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
mu.mem_write(b.base, bytes(b.img))
STK, DATA, RET, GAME = 0x10000000, 0x20000000, 0x30000000, 0x40000000
mu.mem_map(STK, 0x100000); mu.mem_map(DATA, 0x10000); mu.mem_map(RET, 0x1000)
mu.mem_map(GAME, 0x40000)
mu.mem_write(RET, b'\xf4')
# fldcw [DATA]; hlt -- the CRT's _controlfp(_PC_53, _MCW_PC): 0x27f.
mu.mem_write(DATA, struct.pack('<H', 0x27f))
mu.mem_write(RET + 0x10, b'\xd9\x2d' + struct.pack('<I', DATA) + b'\xf4')
mu.reg_write(UC_X86_REG_ESP, STK + 0x80000)
mu.emu_start(RET + 0x10, RET + 0x16)
mu.mem_write(0x511de8, struct.pack('<I', GAME))
GRAVITY = 8155   # 112 * 65536 / 900 (0x48383b's default 0x1fdb)
mu.mem_write(GAME + 0x14263, struct.pack('<i', GRAVITY))


def call(fn, args):
    sp = STK + 0x80000
    for a in reversed(args):
        sp -= 4; mu.mem_write(sp, struct.pack('<I', a & 0xffffffff))
    sp -= 4; mu.mem_write(sp, struct.pack('<I', RET))
    mu.reg_write(UC_X86_REG_ESP, sp)
    mu.emu_start(fn, RET)
    return mu.reg_read(UC_X86_REG_EAX)


def native(dx, dy, dz, v, minangle):
    bits = struct.unpack('<I', struct.pack('<f', minangle))[0]
    return call(0x49a890, [dx, dy, dz, v, bits]) & 0xffff


def model(dx, dy, dz, v, minangle, g=GRAVITY):
    """The engine's port (retailaim.h taBallisticPitch), in doubles."""
    h = math.hypot(float(dx), float(dz))
    gdy = float(g) * float(dy)
    v2 = float(v) * float(v)
    h2 = h * h
    dy2 = float(dy) * float(dy)
    r2 = dy2 + h2
    g2 = float(g * g)
    b_ = (v2 + 2.0 * gdy) * v2
    disc = (b_ + g2 * dy2) * (h2 * h2) - g2 * (h2 * h2) * r2
    if disc < 0: return 0x8000
    s = math.sqrt(disc)
    n = h2 * (v2 + gdy)
    q1, q2 = (n + s) / (2.0 * r2), (n - s) / (2.0 * r2)
    low = math.acos(math.sqrt(q1) / float(v)) if q1 > 0 else math.pi / 2
    high = math.acos(math.sqrt(q2) / float(v)) if q2 > 0 else math.pi / 2
    m = struct.unpack('<f', struct.pack('<f', minangle))[0]
    if m < low and low <= math.pi / 4: a = low
    elif high > m and high <= math.pi / 4: a = high
    else: return 0x8000
    return int(a * 32768.0 * (1.0 / math.pi)) & 0xffff


def sine(angle, r):     # 0x4b70ef / 0x4b7123 stand-ins (the engine uses its tables)
    return int(r * math.sin(angle * math.pi / 32768.0))


def land(dx, dy, dz, v, pitch, g=GRAVITY):
    """Fly 0x49bce3's motion from P toward T (d = P - T) and return the
    shell's height above T as it passes T's horizontal distance."""
    h = math.hypot(dx, dz)
    vh = v * math.cos(pitch * math.pi / 32768.0)
    vy = v * math.sin(pitch * math.pi / 32768.0)
    x = y = 0.0
    for _ in range(100000):
        px, py = x, y
        x += vh; y += vy; vy -= g
        if x >= h:
            return py + (y - py) * (h - px) / (x - px) + dy
    return None


cases = []
for v_px in (350.0, 450.0, 950.0):
    v = int(v_px * 65536 / 30)
    for dist in (200, 500, 800, 1000, 1094, 1100):
        for height in (-60, 0, 40):
            cases.append((dist << 16, height << 16, 0, v, -0.4363323))   # -25 degrees
cases += [((300 << 16), (200 << 16), 0, int(350 * 65536 / 30), -0.4363323),   # far below: |theta|
          (0, 0, (500 << 16), int(450 * 65536 / 30), 0.0),
          ((400 << 16), 0, (-300 << 16), int(450 * 65536 / 30), 0.5)]          # minbarrel above low
mismatch = 0
print(' dx(px)  dy(px)  dz(px)  v(px/s)  minangle  native  model  above T(px)')
for dx, dy, dz, v, m in cases:
    n = native(dx, dy, dz, v, m)
    mm = model(dx, dy, dz, v, m)
    miss = '' if n == 0x8000 else f'{land(dx / 65536, dy / 65536, dz / 65536, v / 65536, n, GRAVITY / 65536):+.1f}'
    flag = '' if n == mm else '  MISMATCH'
    mismatch += n != mm
    print(f'{dx/65536:7.0f} {dy/65536:7.0f} {dz/65536:7.0f} {v*30/65536:8.1f} {m:9.4f}  {n:#06x} {mm:#06x} {miss:>9}{flag}')
print('mismatches', mismatch)
