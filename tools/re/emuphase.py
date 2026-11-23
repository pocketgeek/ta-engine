#!/usr/bin/env python3
"""Drive retail's full pathfinding lifecycle under emulation and OBSERVE the route.

Constructs the real search object (0x415f80), plants a request (grid, unit,
start, goal), runs init (0x415170), then steps the scheduler's phase machine
(+0x5c): phase 1 = tracer (0x4146e0 via 0x415b10), phase 2 = best-first cost
search (0x4142c0), until a route is produced. Observation only.
"""
import os, sys, struct
# This directory, not a fixed checkout: a worktree must import its own modules.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_FPCW

ARENA = 0x40000000
ARENA_SZ = 0x8000000
GS   = ARENA + 0x0800000
CELLS= ARENA + 0x0900000
UNITS= ARENA + 0x0A00000
TYPE = ARENA + 0x0B00000
OBJ  = ARENA + 0x0C00000
BRK0 = ARENA + 0x1000000


class Phase:
    def __init__(self, W=24, H=24):
        self.icd = Icd()
        self.uc = self.icd.uc
        self.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        self.uc.mem_map(ARENA, ARENA_SZ)
        self.W, self.H = W, H
        self.blocked = set()
        self.brk = BRK0
        # The historical fixed CELLS address sits only 1 MiB before UNITS.
        # Keep it for the small path fixtures that write that exported address
        # directly, but place larger map planes in the emulated heap so they do
        # not overwrite the unit/type/object tables.
        self.cells_addr = CELLS
        if W * H * 14 > 0x100000:
            self.cells_addr = self._alloc(W * H * 14)
        self._install_shims()
        self._gamestate()

    def _alloc(self, n):
        if n > 0x400000:
            raise RuntimeError("bogus alloc size %#x (brk=%#x)" % (n, self.brk))
        a = self.brk
        self.brk = (a + max(n, 8) + 15) & ~15
        self.uc.mem_write(a, b"\0" * max(n, 8))
        return a

    def _install_shims(self):
        icd = self.icd

        def one_arg_alloc(uc, argp):   # cdecl: caller cleans, nargs=0
            n = struct.unpack("<I", uc.mem_read(argp, 4))[0]
            return (0, self._alloc(n))

        def two_arg_alloc(uc, argp):
            n = struct.unpack("<I", uc.mem_read(argp + 4, 4))[0]
            return (0, self._alloc(n))

        icd.hooks[0x4eba00] = lambda uc, argp: (0, 0)   # free(ptr) cdecl -> no-op
        icd.hooks[0x4eb9e0] = one_arg_alloc             # malloc(size)
        icd.hooks[0x5ba3d0] = two_arg_alloc             # alloc(tag, size)
        icd.hooks[0x5d3d12] = lambda uc, argp: (0, 0)   # atexit is cdecl; caller cleans
        icd.hooks[0x535cc0] = lambda uc, argp: (1, 0)   # rand(n) -> 0

    def _gamestate(self):
        uc = self.uc
        uc.mem_write(0x62d55c, struct.pack("<I", GS))
        uc.mem_write(0x62d558, struct.pack("<I", GS + 0x600000))
        uc.mem_write(GS, b"\0" * 0x30000)
        g = lambda off, v: uc.mem_write(GS + off, struct.pack("<I", v))
        g(0x19e98, self.W)
        g(0x19e9c, self.H)
        g(0x19f04, self.cells_addr)
        g(0x19edc, UNITS)
        g(0x19ec0, 64)
        uc.mem_write(GS + 0x19ef8, bytes([0]))
        # 413c80 reads a separate explored-map plane. GS+0x100000 is CELLS,
        # so using that address aliases terrain records and exploration words.
        BM = self._alloc(0x40000)
        uc.mem_write(GS + 0x19ef4, struct.pack("<I", BM))
        cell = bytearray(14)
        cell[8:10] = b"\xff\xff"
        uc.mem_write(self.cells_addr, bytes(cell) * (self.W * self.H))
        uc.mem_write(UNITS, b"\0" * (0x140 * 64))
        uc.mem_write(TYPE, b"\0" * 0x400)

    def unit(self, cellx, cellz):
        uc = self.uc
        u = UNITS + 1 * 0x140
        uc.mem_write(u, b"\0" * 0x140)
        uc.mem_write(u + 2, struct.pack("<H", 1))
        uc.mem_write(u + 0xb4, struct.pack("<I", TYPE))
        uc.mem_write(u + 0x78, struct.pack("<h", 0))
        uc.mem_write(u + 8, struct.pack("<I", u + 0x300))
        uc.mem_write(u + 0x300 + 0x36, struct.pack("<H", 0))
        t = TYPE
        uc.mem_write(t + 0x249, bytes([6]))
        uc.mem_write(t + 0x18e, struct.pack("<H", 30))
        uc.mem_write(t + 0x172, struct.pack("<i", 0x13333))
        uc.mem_write(t + 0x16e, struct.pack("<i", 0x10000))
        # player object (unit+0xb8), +0xeb = player index 0
        PLR = ARENA + 0x0E00000
        uc.mem_write(PLR, b"\0" * 0x400)
        uc.mem_write(PLR + 0xeb, bytes([0]))
        uc.mem_write(u + 0xb8, struct.pack("<I", PLR))
        self.U = u
        return u

    def construct(self):
        self.uc.mem_write(OBJ, b"\0" * 0x400)
        return self.icd.call(0x415f80, args=(), ecx=OBJ)[1]

    def plant_request(self, unit, start, goal):
        uc = self.uc
        # Entity +74 is the START footprint origin. The goal controller supplies
        # the destination; older versions accidentally reversed these and used
        # a cell-grade stub for the goal-distance virtual method.
        uc.mem_write(unit + 0x74, struct.pack("<hh", *start))
        uc.mem_write(unit + 0x78, struct.pack("<hh", 1, 1))
        uc.mem_write(unit + 0x130, struct.pack("<I", 0x1000000))
        uc.mem_write(OBJ + 0x58, struct.pack("<I", unit))
        H = ARENA + 0x0D00000
        MISSION = H + 0x1000
        NAV = H + 0x2000
        uc.mem_write(H, b"\0" * 0x14)
        uc.mem_write(H, struct.pack("<IIhhII", 0x5f28d8, MISSION, *goal, 0, 0))
        uc.mem_write(MISSION, b"\0" * 0x72)
        uc.mem_write(MISSION + 0xe, struct.pack("<I", unit))
        uc.mem_write(MISSION + 0x6e, struct.pack("<I", H))
        uc.mem_write(NAV, b"\0" * 0x115)
        uc.mem_write(NAV, struct.pack("<III", 0x5f2a24, H, unit))
        uc.mem_write(OBJ + 0x68, struct.pack("<I", H))
        self.HANDLE, self.NAV, self.GOAL = H, NAV, goal
        # nav-grid object at GRID: dims at +0x340/+0x344, packed 3-bit grade map at
        # +0x348 (index = width*(z>>3)+x, grade = (dword >> (z&7)*4) & 7). Grade 6 =
        # open. This is what the tracer (0x4139d0) and cost search actually read.
        GRID = ARENA + 0x0F00000
        GMAP = ARENA + 0x0F10000
        uc.mem_write(GRID, b"\0" * 0x400)
        uc.mem_write(GRID + 4, struct.pack("<h", 1))   # window x half-extent
        uc.mem_write(GRID + 6, struct.pack("<h", 1))   # window z half-extent
        uc.mem_write(GRID + 0x340, struct.pack("<I", self.W))
        uc.mem_write(GRID + 0x344, struct.pack("<I", self.H))
        uc.mem_write(GRID + 0x348, struct.pack("<I", GMAP))
        rows8 = (self.H + 7) // 8
        # all grade 6 (open): each dword = 6 in all 8 nibble slots
        import array as _a
        words = _a.array("I", [0x66666666] * (self.W * rows8))
        uc.mem_write(GMAP, words.tobytes())
        self.GRID, self.GMAP, self.rows8 = GRID, GMAP, rows8
        for (bx, bz) in self.blocked:
            self._set_grade(bx, bz, 0)
        uc.mem_write(OBJ + 0x64, struct.pack("<I", NAV))   # separate route output
        # give the handle route storage (+0xc..+0x10c) room -- already zeroed in H alloc
        mv = struct.unpack("<I", uc.mem_read(unit + 8, 4))[0]
        uc.mem_write(mv, struct.pack("<I", NAV))
        uc.mem_write(mv + 4, struct.pack("<I", GRID))
        uc.mem_write(OBJ + 0x6c, struct.pack("<I", GRID))

    def attach_live_request(self, unit, mover, nav, handle, start, footprint):
        """Attach the retail search object to an existing unit/nav/controller.

        Unlike plant_request(), this preserves the caller's live mission and
        circle-controller allocation. It is used when a native dispatcher
        creates the request and the fixture then drives retail's search kernel
        at that controlled scheduler boundary.
        """
        uc = self.uc
        W, H = self.W, self.H
        GRID = ARENA + 0x0F00000
        GMAP = ARENA + 0x0F10000
        uc.mem_write(GRID, b"\0" * 0x400)
        uc.mem_write(GRID + 4, struct.pack("<hh", *footprint))
        uc.mem_write(GRID + 0x340, struct.pack("<I", W))
        uc.mem_write(GRID + 0x344, struct.pack("<I", H))
        uc.mem_write(GRID + 0x348, struct.pack("<I", GMAP))
        rows8 = (H + 7) // 8
        import array as _a
        words = _a.array("I", [0x66666666] * (W * rows8))
        uc.mem_write(GMAP, words.tobytes())
        self.GRID, self.GMAP, self.rows8 = GRID, GMAP, rows8
        for (bx, bz) in self.blocked:
            self._set_grade(bx, bz, 0)

        # The caller's controller has already been built by 0x4e2500 and its
        # +4 field already identifies the original unload mission. Keep that
        # identity; only clear the navigator's old route storage.
        uc.mem_write(nav, struct.pack("<III", 0x5f2a24, handle, unit))
        uc.mem_write(nav + 0x0c, bytes(0x109))
        uc.mem_write(mover, struct.pack("<II", nav, GRID))
        uc.mem_write(OBJ + 0x58, struct.pack("<I", unit))
        uc.mem_write(OBJ + 0x64, struct.pack("<I", nav))
        uc.mem_write(OBJ + 0x68, struct.pack("<I", handle))
        uc.mem_write(OBJ + 0x6c, struct.pack("<I", GRID))
        uc.mem_write(unit + 0x74, struct.pack("<hh", *start))
        uc.mem_write(unit + 0x78, struct.pack("<hh", *footprint))
        self.HANDLE, self.NAV, self.GOAL = handle, nav, start

    def _set_grade(self, x, z, g):
        idx = self.W * (z >> 3) + x
        addr = self.GMAP + idx * 4
        import struct as _s
        d = _s.unpack("<I", self.uc.mem_read(addr, 4))[0]
        shift = (z & 7) * 4
        d = (d & ~(0x7 << shift)) | ((g & 7) << shift)
        self.uc.mem_write(addr, _s.pack("<I", d))

    def set_grade_plane(self, grades):
        """Write row-major 0-7 grades into the native packed footprint cache."""
        if len(grades) != self.W * self.H:
            raise ValueError((len(grades), self.W, self.H))
        rows8 = (self.H + 7) // 8
        packed = [0] * (self.W * rows8)
        for z in range(self.H):
            row = z * self.W
            word_row = (z >> 3) * self.W
            shift = (z & 7) * 4
            for x in range(self.W):
                packed[word_row + x] |= (int(grades[row + x]) & 7) << shift
        self.uc.mem_write(self.GMAP, struct.pack("<%dI" % len(packed), *packed))

    def init(self):
        return self.icd.call(0x415170, args=(self.HANDLE,), ecx=OBJ)

    def phase(self):
        return struct.unpack("<i", self.uc.mem_read(OBJ + 0x5c, 4))[0]

    def step(self):
        ph = self.phase()
        if ph == 0:
            self.uc.mem_write(OBJ + 0x48, struct.pack("<i", 0x1f4))
            self.uc.mem_write(OBJ + 0x5c, struct.pack("<i", 1))
            return self.icd.call(0x415170, args=(self.HANDLE,), ecx=OBJ)
        if ph == 1:
            self.uc.mem_write(OBJ + 0x5c, struct.pack("<i", 2))
            return self.icd.call(0x415b10, args=(), ecx=OBJ)
        if ph == 2:
            return self.icd.call(0x4142c0, args=(), ecx=OBJ)
        return (None, "unknown phase %d" % ph)

    def get(self, off, n=4):
        return struct.unpack("<i", self.uc.mem_read(OBJ + off, 4))[0] if n == 4 else \
               struct.unpack("<h", self.uc.mem_read(OBJ + off, 2))[0]


if __name__ == "__main__":
    p = Phase()
    u = p.unit(2, 2)
    print("construct:", "OK" if p.construct() is None else "FAIL")
    p.plant_request(u, start=(2, 2), goal=(20, 20))
    trace = []

    def tr(uc, addr, size, _):
        trace.append(addr)
        if len(trace) > 80:
            trace.pop(0)

    p.uc.hook_add(UC_HOOK_CODE, tr)
    v, e = p.init()
    print("init:", "OK" if e is None else e)
    if e:
        print("last EIPs:", " ".join("%08x" % a for a in trace[-24:]))
    else:
        for off in (0x30, 0x34, 0xbc, 0xc0, 0xc4, 0xc8, 0xb4, 0xb8, 0xec, 0x5c):
            print("  OBJ+%#x = %#x" % (off, p.get(off) & 0xffffffff))
