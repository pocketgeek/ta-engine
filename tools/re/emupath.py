"""Emulate retail's per-cell passability query (0x4db640) against synthetic state.

The query the tracer scores every cell with (0x4139d0 -> 0x413c80 -> 0x4db640).
Builds a minimal game state: cell grid, unit table, one unit-under-test, and an
optional occupant, then enumerates configurations -> the exact grade table.
Observation only; nothing is copied out of the binary.
"""
import os, struct, sys
# This directory, not a fixed checkout: a worktree must import its own modules.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

U = None

class World:
    GS   = HEAP + 0x10000
    CELLS= HEAP + 0x80000     # cell records, 14 bytes each
    UNITS= HEAP + 0x100000    # unit table, stride 0x140
    TYPES= HEAP + 0x180000    # unit type records
    W = 32; H = 32

    def __init__(self, icd):
        self.icd = icd; uc = icd.uc
        uc.mem_write(0x62d55c, struct.pack("<I", self.GS))
        uc.mem_write(self.GS, b"\0" * 0x30000)
        g = lambda off, v: uc.mem_write(self.GS + off, struct.pack("<I", v))
        g(0x19e98, self.W); g(0x19e9c, self.H)
        g(0x19f04, self.CELLS)
        g(0x19edc, self.UNITS); g(0x19ec0, 64)
        uc.mem_write(self.GS + 0x19ef8, bytes([0]))     # waterline: 0 = everything is dry land
        self.clear()

    def clear(self):
        uc = self.icd.uc
        # every cell: no feature (+0), no occupant (+8 = 0xffff), flat height
        rec = struct.pack("<HHHHHHH", 0, 0, 0, 0, 0xFFFF, 0, 0)
        # layout guess: [+0]=feature id, [+5]/[+6] height bytes, [+8]=occupant,
        # [+0xa]/[+0xb] backptr, [+0xc] flags. Build byte-wise instead:
        cell = bytearray(14)
        cell[8:10] = b"\xff\xff"
        uc.mem_write(self.CELLS, bytes(cell) * (self.W * self.H))
        uc.mem_write(self.UNITS, b"\0" * (0x140 * 64))
        uc.mem_write(self.TYPES, b"\0" * 0x4000)

    def mktype(self, idx, foot=(1,1)):
        uc = self.icd.uc
        t = self.TYPES + idx * 0x400
        uc.mem_write(t, b"\0" * 0x400)
        # limits wide open: slope bytes +0x10..0x13 = 127, heights +0x192/+0x194 = 0
        uc.mem_write(t + 0x10, bytes([127,127,127,127]))
        uc.mem_write(t + 0xc, struct.pack("<hh", foot[0], foot[1]))   # +0xc/+0xe: extents?
        uc.mem_write(t + 0x249, bytes([6]))
        return t

    def mkunit(self, slot, cellx, cellz, typ, moving=False, heading=0, speed=0):
        uc = self.icd.uc
        u = self.UNITS + slot * 0x140
        uc.mem_write(u, b"\0" * 0x140)
        uc.mem_write(u + 2, struct.pack("<H", slot))
        uc.mem_write(u + 0xb4, struct.pack("<I", typ))
        flags = 0x20 | (0x20000 if moving else 0)
        uc.mem_write(u + 0x13c, struct.pack("<I", flags))
        uc.mem_write(u + 0x78, struct.pack("<hh", cellx, cellz))
        uc.mem_write(u + 0x130, struct.pack("<I", 0))
        return u

    def occupy(self, cellx, cellz, slot):
        uc = self.icd.uc
        rec = self.CELLS + (cellz * self.W + cellx) * 14
        uc.mem_write(rec + 8, struct.pack("<H", slot))

def query(icd, unit, wx_cell, wz_cell):
    # world coords in the 20.12-ish fixed the caller uses: cell<<20 + half
    wx = (wx_cell << 20) + 0x80000
    wz = (wz_cell << 20) + 0x80000
    v, err = icd.call(0x4db640, args=(unit, wx, 0, wz))
    return v, err

def install_rt(icd):
    """Runtime shims: atexit -> no-op, the malloc family -> a bump allocator
    (their lock init calls through the unmapped Windows IAT), free -> no-op."""
    icd.hooks[0x5d3d12] = lambda uc, argp: (0, 0)          # atexit
    state = {"brk": HEAP + 0x200000}
    def _alloc(uc, argp):
        import struct as _s
        n = _s.unpack("<I", uc.mem_read(argp, 4))[0]
        a = state["brk"]; state["brk"] = (a + max(n, 4) + 15) & ~15
        uc.mem_write(a, b"\0" * max(n, 4))
        return (0, a)                                       # cdecl: caller cleans
    for f in (0x4eb9e0, 0x5ba690, 0x5ba3e0):                # new / malloc chain
        icd.hooks[f] = _alloc
    for f in (0x4eba00,):                                   # free
        icd.hooks[f] = lambda uc, argp: (0, 0)

if __name__ == "__main__":
    icd = Icd()
    install_rt(icd)
    w = World(icd)
    t = w.mktype(0)
    me = w.mkunit(1, 5, 5, t)
    print("empty cell:", query(icd, me, 8, 5))
    w.occupy(8, 5, 2)
    w.mkunit(2, 8, 5, t, moving=False)
    print("parked occupant:", query(icd, me, 8, 5))
    w.mkunit(2, 8, 5, t, moving=True)
    print("moving occupant:", query(icd, me, 8, 5))

def rate(icd, w, rater, unit, cellx, cellz):
    """Call one of the three raters on a cell. Their common shape:
    0x508660(unit, cellRec*)          -- ret 8
    0x5088f0(unit, x, z, w?, h?)      -- footprint window: use x,z,1,1
    0x509020(unit?, cellRec*, x, cnt) -- per the 0x508fc5 call site
    """
    rec = w.CELLS + (cellz * w.W + cellx) * 14
    if rater == 0x508660:
        return icd.call(rater, args=(unit, rec))
    if rater == 0x5088f0:
        return icd.call(rater, args=(unit, cellx, cellz, 1, 1))
    if rater == 0x509020:
        return icd.call(rater, args=(unit, rec, cellx, 1))
    raise ValueError

def matrix(icd):
    w = World(icd)
    t = w.mktype(0)
    cases = [
        ("empty",            None),
        ("parked",           dict(moving=False)),
        ("moving same-way",  dict(moving=True, heading=0)),
        ("moving opposite",  dict(moving=True, heading=0x8000)),
    ]
    for rater in (0x508660, 0x5088f0, 0x509020):
        print(f"--- rater {hex(rater)} ---")
        for name, occ in cases:
            w.clear()
            me = w.mkunit(1, 5, 5, t)
            if occ is not None:
                w.mkunit(2, 8, 5, t, **{k: v for k, v in occ.items() if k != "heading"})
                if "heading" in occ:
                    icd.uc.mem_write(w.UNITS + 2*0x140 + 0x7e, struct.pack("<H", occ["heading"]))
                w.occupy(8, 5, 2)
            v, err = rate(icd, w, rater, me, 8, 5)
            print(f"  {name:18s} -> {v if err is None else 'ERR ' + err[:50]}")
