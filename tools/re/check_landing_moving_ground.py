#!/usr/bin/env python3
"""Does retail 509400 see a MOVING ground unit?

A ground unit G walks east across a flat explored map. Each tick its
position is committed natively through the position setter 51b3b0
(506aa0 unstamp + 5066f0 stamp on a footprint/mode change, the same pair the
mover commit 4dafd2..4db001 runs). Every tick a hovering flyer F asks 509400
about three sites: G's current footprint, G's previous footprint, and one
cell ahead of G. We also vary G's mover/navigator state (+0xb8 record,
+0x130 flags bit 0x4000 'moved') to show 509400 ignores it.
Finally: 507d10 (the ground mover's cell-entry test) against a cell that
holds only a DESCENDING flyer (mode 2, airborne word +2) vs a landed one.
"""
import struct, sys
from emu import Icd, HEAP

p = Icd()
game, cells, explore, pool, ftyp, gtyp, point, nav = (HEAP + o for o in
    (0, 0x20000, 0x30000, 0x40000, 0x60000, 0x61000, 0x62000, 0x63000))
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def w16(a, v): p.uc.mem_write(a, struct.pack('<H', v & 0xffff))
def r16(a): return struct.unpack('<H', bytes(p.uc.mem_read(a, 2)))[0]
def r32(a): return struct.unpack('<I', bytes(p.uc.mem_read(a, 4)))[0]
p.hooks[0x506650] = lambda uc, sp: (2, 0)   # sector relink
p.hooks[0x4e1e60] = lambda uc, sp: (1, 0)   # post-unstamp notify
p.hooks[0x4e1e20] = lambda uc, sp: (2, 0)
p.freeze_hooks()
put(0x62d55c, game)
def call(addr, args):
    eax, err = p.call(addr, args, timeout=0); assert err is None, err; return eax

W, H = 24, 12
COUNT = 3
words = (W >> 1) * ((H >> 1) + 4)
p.uc.mem_write(game, bytes(0x1a000))
put(game + 0x19e98, W); put(game + 0x19e9c, H); put(game + 0x19f04, cells)
put(game + 0x19ef4, explore); put(game + 0x14e84, pool); put(game + 0x14e88, pool + COUNT * 0x138)
put(game + 0x19f30, 0xdead0000)
p.uc.mem_write(cells, (bytes(8) + b'\xff\xff' + bytes(4)) * (W * H))
p.uc.mem_write(explore, struct.pack(f'<{words}H', *([0xffff] * words)))
p.uc.mem_write(pool, bytes((COUNT + 1) * 0x138))
for t, flags in ((ftyp, 0x800), (gtyp, 0)):
    p.uc.mem_write(t, bytes(0x270))
    w16(t + 0x192, 10000); w16(t + 0x194, -10000 & 0xffff)
    p.uc.mem_write(t + 0x23c, bytes([255, 255])); put(t + 0x260, flags)
    p.uc.mem_write(t + 0x24a, bytes([1])); p.uc.mem_write(t + 0x126, struct.pack('<2h', 2, 2))
p.uc.mem_write(nav, bytes(0x200))

def unit(i, typ, f):
    a = pool + i * 0x138
    w16(a + 2, i); p.uc.mem_write(a + 0x78, struct.pack('<2h', f, f)); put(a + 0xb4, typ)
    put(a + 0xb8, nav); put(a + 0xa4, 0x1234)
    p.uc.mem_write(a + 0x126, struct.pack('<2h', -9999, -9999))
    return a
F = unit(1, ftyp, 2)          # the would-be lander
G = unit(2, gtyp, 2)          # moving ground unit, 2x2
D = unit(3, ftyp, 2)          # another flyer for the 507d10 test
put(F + 0x130, 0x1000000 | 2)

def fp(x, f): return ((x - (f << 19) + 0x80000) >> 20)
def site_free(cx, cz, f=2):
    x = (cx * 16 + f * 8) << 16; z = (cz * 16 + f * 8) << 16
    p.uc.mem_write(point, struct.pack('<iii', x, 0, z))
    return call(0x509400, (F, point)) & 1

# Initial placement of G at x=40px (cell 1..2), mode 1, then walk east 3 px/tick.
gx, gz = 40 << 16, 48 << 16
put(G + 0x130, 0x1000000 | 1)
p.uc.mem_write(G + 0x74, struct.pack('<2h', fp(gx, 2), fp(gz, 2)))
put(G + 0x68, gx); put(G + 0x70, gz); call(0x5066f0, (G,))
out = []
mismatch = 0
prev = fp(gx, 2)
for tick in range(40):
    gx += 3 << 16
    # vary 'moving' state the mover would have: navigator record live, +0x130 bit 0x4000
    p.uc.mem_write(nav + 0xea, bytes([1 + (tick & 1)])); put(nav, tick & 1)
    call(0x51b3b0, (G, gx, 0, gz, 1))
    cur = fp(gx, 2); cz = fp(gz, 2)
    row = [r16(cells + (cz * W + c) * 14) for c in range(W)]
    a = site_free(cur, cz); b = site_free(prev, cz) if prev != cur else None
    ahead = site_free(cur + 2, cz)
    out.append(f"t{tick:02d} Gx={gx>>16:3d}px cell={cur:2d} flags130={r32(G+0x130):08x} "
               f"word0[row]={''.join(str(v) if v else '.' for v in row)} "
               f"509400(on G)={a} (G's old cell {prev})={b} (ahead {cur+2})={ahead}")
    if a != 0 or ahead != 1 or (b is not None and b != (1 if abs(prev - cur) >= 2 else 0)): mismatch += 1
    prev = cur
print('\n'.join(out))
print('UNEXPECTED rows:', mismatch)

# 507d10: can ground unit G enter cells holding flyer D (descending: only word +2; landed: word +0)?
for c in range(W * H): w16(cells + c * 14, 0); w16(cells + c * 14 + 2, 0)
dx, dz = 10, 2
p.uc.mem_write(D + 0x74, struct.pack('<2h', dx, dz))
put(D + 0x130, 0x1000000 | 2)
for r in range(2):
    for c in range(2): w16(cells + ((dz + r) * W + dx + c) * 14 + 2, 3)   # airborne word +2
cell = (dz & 0xffff) << 16 | (dx & 0xffff)
desc = call(0x507d10, (gtyp, 2, cell, 1, 0))
put(D + 0x130, 0x1000000 | 1); call(0x5066f0, (D,))
landed = call(0x507d10, (gtyp, 2, cell, 1, 0))
print(f"507d10 ground entry onto descending flyer (word+2 only) = {desc & 1}; onto landed flyer (word+0) = {landed & 1}")
sys.exit(1 if mismatch or (desc & 1) != 1 or (landed & 1) != 0 else 0)
