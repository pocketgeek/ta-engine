"""Unicorn harness: dynamic confirmation of TotalA.exe ground pathfinding (observe only).

Runs the retail per-cell grade (0x47de60), the per-movement-class grade plane
build (0x440470 + 0x440500), and the path search (init 0x40e630 + its tracer
0x40e160, the A* pop/expand body 0x40df00/0x40da70, reconstruction 0x40e050)
on synthetic maps, and compares every result with path_model.py, an
independent Python reading of the same routines.  Nothing from the binary is
copied into engine source.

Usage: path_emu.py [grade|plane|search|rect|all] [trials] [seed]
"""
import sys, struct, random
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import pe
import path_model as M
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn.x86_const import *

EXE = '/home/pocket_geek/ta_install/TotalA.exe'
b = pe.Bin(EXE)
GAME, HEAP, STACK, STOP = 0x10000000, 0x20000000, 0x30000000, 0x34000000
HEAPSZ = 0x4000000


class Emu:
    def __init__(self):
        u = self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(b.base, (len(b.img) + 0xfff) & ~0xfff)
        u.mem_write(b.base, bytes(b.img))
        for a, n in ((GAME, 0x40000), (HEAP, HEAPSZ), (STACK, 0x100000), (STOP, 0x1000)):
            u.mem_map(a, n)
        u.mem_write(STOP, b'\xf4')
        self.w32(0x511de8, GAME)
        self.heap = HEAP
        self.routes = []
        # cdecl allocator stubs: name -> (nargs to read, handler)
        self.stubs = {
            0x4b4f10: lambda a: self.alloc(a[0]),          # malloc(size)
            0x4b4f20: lambda a: 0,                          # free(p)
            0x4d83b0: lambda a: self.alloc(a[1]),          # tagged alloc(name, size)
            0x4d85a0: lambda a: 0,                          # tagged free
        }
        u.hook_add(UC_HOOK_CODE, self._code)
        u.hook_add(UC_HOOK_MEM_UNMAPPED, self._bad)

    def _bad(self, u, access, addr, size, value, _):
        print('  unmapped access', hex(addr), 'eip', hex(u.reg_read(UC_X86_REG_EIP)))
        return False

    def _code(self, u, addr, size, _):
        if addr in self.stubs:
            esp = u.reg_read(UC_X86_REG_ESP)
            args = [self.r32(esp + 4 + 4 * i) for i in range(3)]
            u.reg_write(UC_X86_REG_EAX, self.stubs[addr](args))
            u.reg_write(UC_X86_REG_ESP, esp + 4)
            u.reg_write(UC_X86_REG_EIP, self.r32(esp))
        elif addr == 0x44f080:            # navigator deliver(points, count), thiscall ret 8
            esp = u.reg_read(UC_X86_REG_ESP)
            pts, n = self.r32(esp + 4), self.r32(esp + 8)
            route = [(self.r16(pts + 4 * i), self.r16(pts + 4 * i + 2)) for i in range(n)]
            self.routes.append(route)
            u.reg_write(UC_X86_REG_ESP, esp + 12)
            u.reg_write(UC_X86_REG_EIP, self.r32(esp))

    def alloc(self, n):
        a = self.heap
        self.heap += (n + 0x1f) & ~0xf
        assert self.heap < HEAP + HEAPSZ
        return a

    def w8(self, a, v): self.u.mem_write(a, bytes([v & 0xff]))
    def w16(self, a, v): self.u.mem_write(a, struct.pack('<H', v & 0xffff))
    def w32(self, a, v): self.u.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def r8(self, a): return self.u.mem_read(a, 1)[0]
    def r32(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def r16(self, a): return struct.unpack('<h', self.u.mem_read(a, 2))[0]

    def call(self, fn, args, ecx=0):
        u = self.u
        esp = STACK + 0xf0000
        for a in reversed(args):
            esp -= 4; self.w32(esp, a)
        esp -= 4; self.w32(esp, STOP)
        u.reg_write(UC_X86_REG_ESP, esp)
        u.reg_write(UC_X86_REG_ECX, ecx)
        u.emu_start(fn, STOP, count=200_000_000)
        return u.reg_read(UC_X86_REG_EAX)


# ------------------------------------------------------------------ world setup
def setup_world(e, m):
    """m: path_model.Map. Writes the game-object fields the routines read."""
    w, h = m.w, m.h
    G = GAME
    e.w32(G + 0x14233, w); e.w32(G + 0x14237, h)
    e.w8(G + 0x1427f, m.sea)
    cells = e.alloc(w * h * 13)
    for z in range(h):
        for x in range(w):
            c = cells + (z * w + x) * 13
            lo, hi = m.lohi(x, z)
            e.w16(c + 0, m.unit[z][x])
            e.w8(c + 4, m.height[z][x]); e.w8(c + 5, hi); e.w8(c + 6, lo)
            e.w16(c + 8, m.feature[z][x])
    e.w32(G + 0x14287, cells)
    # one feature definition (index 0) whose flags byte +0xfe carries "blocking" (bit 6)
    feat = e.alloc(0x100 * 2)
    e.w8(feat + 0xfe, 0x40)          # feature 0: blocking
    e.w8(feat + 0x100 + 0xfe, 0x00)  # feature 1: not blocking
    e.w32(G + 0x1426f, feat); e.w32(G + 0x14253, 2)
    # unit array: index 0 unused; units 1.. from the model, 0x118 bytes each
    nunits = len(m.units)
    ua = e.alloc(0x118 * (nunits + 2))
    e.w32(G + 0x14357, ua)
    e.w32(G + 0x1435b, ua + 0x118 * max(nunits, 0))   # last allocated record
    for i, (hasmover, stamp) in enumerate(m.units, start=1):
        rec = ua + 0x118 * i
        if hasmover:
            mv = e.alloc(0x40); e.w32(mv + 0x26, stamp); e.w32(rec, mv)
        else:
            e.w32(rec, 0)
        e.w32(rec + 0x110, 0)        # flag 0x10000000 clear: never refreshed by 0x440af0
    # visibility plane (32px), one u16 per cell, player bits: all explored
    vw, vh = w // 2, h // 2
    vis = e.alloc(max(1, vw * vh) * 2)
    for i in range(vw * vh):
        e.w16(vis + 2 * i, m.visible_bits(i % vw, i // vw))
    e.w32(G + 0x14273, vis)
    e.w32(G + 0x38a47, m.tick)
    return cells


def make_class(e, m, cls):
    rec = e.alloc(0x20)
    e.w32(rec, 0x505484)                 # any non-null name pointer
    e.w16(rec + 4, cls.fx); e.w16(rec + 6, cls.fz)
    e.w16(rec + 8, cls.maxdepth); e.w16(rec + 0xa, cls.mindepth)
    e.w8(rec + 0xc, cls.maxslope); e.w8(rec + 0xd, cls.badslope)
    e.w8(rec + 0xe, cls.maxwslope); e.w8(rec + 0xf, cls.badwslope)
    e.w32(rec + 0x1c, cls.stamp)
    return rec


def read_plane(e, rec, w, h):
    base = e.r32(rec + 0x18)
    out = [[0] * w for _ in range(h)]
    for z in range(h):
        for x in range(w):
            word = e.r32(base + 4 * ((z >> 4) * w + x))
            out[z][x] = (word >> ((z & 15) * 2)) & 3
    return out


# ------------------------------------------------------------------ tests
def test_grade(trials):
    rng = random.Random(1); hist = {}
    bad = 0
    for t in range(trials):
        e = Emu()
        m = M.Map.random(rng, 4, 4, density=0.5)
        cls = M.MoveClass.random(rng)
        cells = setup_world(e, m)
        rec = make_class(e, m, cls)
        for z in range(m.h):
            for x in range(m.w):
                got = e.call(0x47de60, [rec, cells + (z * m.w + x) * 13])
                want = M.raw_grade(m, cls, x, z); hist[got] = hist.get(got, 0) + 1
                if got != want:
                    bad += 1
                    if bad < 10: print('grade mismatch', t, x, z, m.lohi(x, z), m.sea, cls, got, want)
    print(f'grade (0x47de60): {trials} maps x 16 cells, grades {hist}, mismatches {bad}')
    return bad


def test_plane(trials):
    rng = random.Random(2)
    bad = 0
    for t in range(trials):
        e = Emu()
        m = M.Map.random(rng, rng.randint(6, 20), rng.randint(6, 20))
        cls = M.MoveClass.random(rng, foot=(rng.randint(1, 4), rng.randint(1, 4)))
        setup_world(e, m)
        rec = make_class(e, m, cls)
        e.call(0x440470, [m.w, m.h], ecx=rec)
        e.call(0x440500, [], ecx=rec)
        got = read_plane(e, rec, m.w, m.h)
        want = M.class_plane(m, cls)
        if got != want:
            bad += 1
            if bad < 4:
                print('plane mismatch', t, cls)
                for z in range(m.h): print(''.join(map(str, got[z])), ''.join(map(str, want[z])))
    print(f'plane (0x440500): {trials} maps, mismatches {bad}')
    return bad


def run_search(e, m, cls, start, goal, radius_px, heading, weight):
    """Drive one TA search the way 0x40eb70 does; returns (route, notify, model-comparable)."""
    rec = make_class(e, m, cls)
    e.call(0x440470, [m.w, m.h], ecx=rec)
    e.call(0x440500, [], ecx=rec)
    pf = e.alloc(0xd0)
    e.call(0x40e9e0, [], ecx=pf)
    # unit, mover, navigator stand-in, goal (circle controller, vtable 0x4fd328)
    unit = e.alloc(0x118); mover = e.alloc(0x40); nav = e.alloc(0x70)
    e.w32(unit, mover); e.w32(mover + 4, rec); e.w32(mover + 0x26, m.tick)
    e.w16(unit + 0x76, start[0]); e.w16(unit + 0x78, start[1])
    e.w16(unit + 0x7e, cls.fx); e.w16(unit + 0x80, cls.fz)
    e.w16(unit + 0x66, heading)
    goalobj = e.alloc(0x20)
    e.w32(goalobj, 0x4fd328); e.w32(goalobj + 4, unit)
    e.w16(goalobj + 8, goal[0]); e.w16(goalobj + 0xa, goal[1])
    e.w32(goalobj + 0xc, radius_px)
    rc = int(radius_px / 16) if radius_px >= 0 else -int(-radius_px / 16)
    e.w32(goalobj + 0x10, rc * rc)
    e.w32(pf + 0x58, unit); e.w32(pf + 0x5c, nav); e.w32(pf + 0x50, weight)
    e.w8(pf + 0x78, 0)
    e.routes = []
    e.call(0x40e630, [goalobj], ecx=pf)
    notify = e.r32(unit + 0x4e)
    if e.routes:                         # init delivered (unreachable / already there)
        return e.routes[0], notify, e.r32(pf + 0x40)
    first = True
    pops = 0
    while True:
        if e.r32(pf + 0x14) - e.r32(pf + 0x18) == 0:
            e.call(0x44f080, [0, 0], ecx=nav)  # 0x40eec9: heap empty -> empty route
            break
        found = e.call(0x40df00, [], ecx=pf)
        pops += 1
        if first:
            e.w32(pf + 0x44, 2); first = False   # 0x40eea8: fan narrows after the first pop
        if found:
            e.call(0x40e050, [], ecx=pf)
            break
    return e.routes[0], notify, e.r32(pf + 0x40)


def test_search(trials):
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 3)
    bad = 0
    stats = {}
    for t in range(trials):
        e = Emu()
        w, h = rng.randint(8, 28), rng.randint(8, 28)
        m = M.Map.random(rng, w, h, density=rng.choice([0.0, 0.1, 0.25, 0.4]),
                         noise=rng.choice([1, 3, 6]))
        f = rng.choice([1, 1, 2, 3])
        if rng.random() < 0.7:
            ms = rng.choice([10, 16, 24])
            cls = M.MoveClass(f, f, maxslope=ms, badslope=ms // 2, maxwslope=ms, badwslope=ms // 2)
        else:
            cls = M.MoveClass.random(rng, foot=(f, f))
        start = (rng.randrange(w), rng.randrange(h))
        goal = (rng.randrange(w), rng.randrange(h))
        radius = rng.choice([0, 16, 20, 48])
        heading = rng.randrange(65536)
        weight = rng.choice([0x18000, 0x48000, 0x90000])
        setup_world(e, m)
        route, notify, gr = run_search(e, m, cls, start, goal, radius, heading, weight)
        plane = M.class_plane(m, cls)
        want = M.search(m, cls, plane, start, goal, radius, heading, weight)
        got = (route, notify & 0x300, gr)
        exp = (want['route'], want['notify'], want['radius'])
        kind = want['kind']
        stats[kind] = stats.get(kind, 0) + 1
        if got != exp:
            bad += 1
            if bad < 5:
                print('search mismatch', t, w, h, cls, start, goal, radius, heading, hex(weight))
                print('  emu  ', got)
                print('  model', exp)
    print(f'search (0x40e630/0x40e160/0x40df00/0x40e050): {trials} cases {stats}, mismatches {bad}')
    return bad


def test_rect(trials):
    """0x47e1f0 (the per-cell re-rate 0x440830 uses for a refreshed rectangle)
    must agree with the full plane build for every cell."""
    rng = random.Random(4)
    bad = differs = 0
    for t in range(trials):
        e = Emu()
        m = M.Map.random(rng, rng.randint(6, 16), rng.randint(6, 16), noise=rng.choice([2, 8]))
        cls = M.MoveClass.random(rng, foot=(rng.randint(1, 4), rng.randint(1, 4)))
        setup_world(e, m)
        rec = make_class(e, m, cls)
        e.call(0x440470, [m.w, m.h], ecx=rec)
        full = M.class_plane(m, cls)
        for z in range(m.h):
            for x in range(m.w):
                got = e.call(0x47e1f0, [rec, x, z])
                want = M.refresh_grade(m, cls, x, z)
                if got != full[z][x]: differs += 1
                if got != want:
                    bad += 1
                    if bad < 6: print('rect mismatch', t, x, z, cls, got, want)
    print(f'rect re-rate (0x47e1f0): {trials} maps, mismatches {bad}; '
          f'cells where the local re-rate differs from the full build 0x440500: {differs}')
    return bad



if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    tot = 0
    if what in ('grade', 'all'): tot += test_grade(n or 60)
    if what in ('plane', 'all'): tot += test_plane(n or 60)
    if what in ('search', 'all'): tot += test_search(n or 150)
    if what in ('rect', 'all'): tot += test_rect(n or 40)
    print('TOTAL mismatches', tot)
