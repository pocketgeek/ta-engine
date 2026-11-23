"""A random AI manager for the Classic oracles of the base think's transfers
and expansion (check_ai_transfer.py, check_ai_expand.py): one player record,
its manager with all 98 squads (retail's own squad-data init 0x50b340), up to 40
units spread over a few bases (ground, air and naval combat units, builders free
or busy, structures, the Monarch; live, dying, selectable, finished or not,
held, carried; hp around 3/4), their movers and the navigator grids those point
at with zone labels per start and sacred-site record, the two anchor tables,
and a visible-enemy list. It writes all of it into the emulator and returns
the matching text fields for classic_ai_test.

Observation only: nothing from the binary is copied into the engine.
"""
import struct

from emu import HEAP

GAME = HEAP                  # +0x2404 player records, +0x19f44 tick
MGR = HEAP + 0x22400
BM = HEAP + 0x22600          # build manager: +9/+0xd visible-enemy list, +0x79
SQOBJ = HEAP + 0x23000
DATA = HEAP + 0x24000
VEC = HEAP + 0x29000
UNITS = HEAP + 0x30000
TYPES = HEAP + 0x40000
MISSIONS = HEAP + 0x4c000
MOVERS = HEAP + 0x50000
HOLDER, NONHOLDER = HEAP + 0x51000, HEAP + 0x51200
DESC = HEAP + 0x52000
STARTS, SITES = HEAP + 0x53000, HEAP + 0x54000
NAVS = HEAP + 0x55000        # three grids, 0x400 apart: +0x1c / +0x2c label vectors
LABELS = HEAP + 0x58000
ENEMIES, LIST = HEAP + 0x60000, HEAP + 0x5f000
ARENA = HEAP + 0x100000
USTRIDE, TSTRIDE, DSTRIDE, CAP, RSTRIDE = 0x138, 0x2a4, 0xc4, 64, 0x110
VTABLE = {'base': 0x5ebb2c, 'strike': 0x5ebb50, 'backup': 0x5ebb60, 'vtol': 0x5ebb70, 'reserve': 0x5ebb1c}


def kind(slot):
    if slot <= 20: return 'base'
    if slot <= 80: return 'strike' if slot % 2 else 'backup'
    if slot <= 95: return 'vtol'
    return {96: 'reserve', 97: 'strike', 98: 'backup'}[slot]


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xffffffff))[0]


class Env:
    def __init__(self, p):
        self.p, self.uc = p, p.uc
        self.heap = [ARENA]
        self.log = []

    def put(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def geti(self, a): return struct.unpack('<i', self.uc.mem_read(a, 4))[0]
    def index(self, address): return (address - UNITS) // USTRIDE if address else 0

    def hooks(self):
        def new(uc_, argv):
            size = self.get(argv); at = self.heap[0]; self.heap[0] = (at + size + 15) & ~15
            return 0, at
        def clear(uc_, argv):
            u, arg = struct.unpack('<II', uc_.mem_read(argv, 8))
            self.log.append((4, self.index(u), arg)); return 2, 0
        return {0x4eb9e0: new, 0x4eba00: lambda uc_, argv: (0, 0), 0x4d6a50: clear}

    def build(self, rng, focus=None):
        """One random manager; `focus` is the base whose members are the
        point of the case (it gets the most units)."""
        uc, put = self.uc, self.put
        self.heap[0] = ARENA; self.log.clear()
        uc.mem_write(GAME, bytes(0x20000)); uc.mem_write(MGR, bytes(0x1b0)); uc.mem_write(BM, bytes(0x100))
        for base, size in ((UNITS, 0x6000), (TYPES, 0xb000), (MISSIONS, 0x2000), (MOVERS, 0x400), (NAVS, 0xc00)):
            uc.mem_write(base, bytes(size))
        put(0x62d55c, GAME)
        put(HOLDER + 0x130, 0x4000000 | 0x1000000); put(NONHOLDER + 0x130, 0x1000000)
        own = rng.randrange(10)
        PLAYER = GAME + 0x2404 + own * RSTRIDE
        side = own
        for s in range(10): put(0x62a33c + 4 * s, BM if s == side else 0)
        put(MGR, PLAYER); uc.mem_write(MGR + 4, bytes([side]))
        put(PLAYER + 0x84, DATA)
        uc.mem_write(DATA, bytes(100 * DSTRIDE))
        for slot in range(100):
            _, error = self.p.call(0x50b340, (PLAYER, slot), ecx=DATA + slot * DSTRIDE)
            if error: raise RuntimeError(error)
        for slot in range(100):
            obj = SQOBJ + slot * 0x20
            if slot in (0, 99):
                put(MGR + 0x11 + slot * 4, 0); continue
            put(obj, VTABLE[kind(slot)]); put(obj + 4, MGR); put(obj + 8, DATA + slot * DSTRIDE)
            put(obj + 12, 0); put(obj + 16, side)
            put(MGR + 0x11 + slot * 4, obj)

        def anchors(base, pointer, count):
            out = []
            for k in range(count):
                valid = int(rng.random() < 0.9)
                x, z = rng.randrange(0, 200), rng.randrange(0, 200)
                if out and rng.random() < 0.2: x, z = out[-1][1], out[-1][2]   # an equal-distance pair
                uc.mem_write(base + k * 101, bytes([valid]) + struct.pack('<hh', x, z) + bytes(96))
                out.append((valid, x, z))
            put(pointer, base if out or rng.random() < 0.5 else 0); put(pointer + 4, base + len(out) * 101 if out else self.get(pointer))
            return out
        sites = anchors(SITES, 0x62dbcc, rng.choice([0, 1, 2, 4, 8, 12]))
        starts = anchors(STARTS, 0x62dbdc, rng.choice([0, 1, 4, 10, 10]))
        self.sites, self.starts = sites, starts

        # Navigator grids: a label per record, now and then one too many or too few.
        navs = []
        for k in range(3):
            nav = NAVS + k * 0x400
            lists = []
            for at, n in ((0x1c, len(starts)), (0x2c, len(sites))):
                if rng.random() < 0.05: n = max(0, n + rng.choice([-1, 1]))
                labels = [rng.choice([-1, 1, 1, 2, 2, 3, 4]) for _ in range(n)]
                begin = LABELS + k * 0x400 + (0 if at == 0x1c else 0x200)
                for j, v in enumerate(labels): put(begin + 4 * j, v)
                put(nav + at, begin if n or rng.random() < 0.5 else 0); put(nav + at + 4, begin + 4 * n if n else self.get(nav + at))
                lists.append(labels)
            navs.append(lists)
        self.navs = navs

        bases = sorted({focus} | {rng.randrange(1, 21) for _ in range(rng.choice([0, 1, 2, 3, 5]))}) if focus else []
        params = {}
        for slot in range(1, 21):
            d = DATA + slot * DSTRIDE
            # Anchors name a record of the tables (retail reads a label vector
            # unchecked at the anchor's index); the record may be empty.
            records = [k + 1 for k in range(len(starts))] + [-(k + 1) for k in range(len(sites))]
            if records and rng.random() < (0.85 if slot in bases else 0.3): p0 = rng.choice(records)
            else: p0 = 0
            q = [p0, rng.randrange(0, 5), rng.randrange(0, 6), rng.randrange(0, 5), rng.randrange(0, 4)]
            q += [rng.choice([1, 2, 3, 5]) * rng.choice([1, -1]) for _ in range(3)] + [rng.randrange(5)]
            for k, v in enumerate(q): put(d + 0xc + 4 * k, v)
            params[slot] = q

        nunits = rng.choice([1, 3, 6, 10, 16, 24, 40])
        put(PLAYER + 0x74, UNITS + USTRIDE); put(PLAYER + 0x78, UNITS + nunits * USTRIDE)
        cx, cz = rng.randrange(500, 3000), rng.randrange(500, 3000)
        units, classes = [], {}
        members = {s: [] for s in range(1, 99)}
        roles = ['ground'] * 5 + ['air'] * 2 + ['naval'] + ['builder'] * 4 + ['structure', 'monarch', 'other']
        for i in range(1, nunits + 1):
            u, t = UNITS + i * USTRIDE, TYPES + i * TSTRIDE
            role = rng.choice(roles)
            flags = 0x1000000 if rng.random() < 0.97 else 0
            if rng.random() < 0.04: flags |= 0x1000
            if rng.random() < 0.9: flags |= 0x20
            if role == 'structure': flags |= 0x2000000
            if role in ('ground', 'air', 'naval', 'monarch') or rng.random() < 0.2: flags |= 0x8000000
            flags |= 1
            tflags = (0x800 if role == 'air' else 0) | (0x80000 if role == 'naval' else 0) | \
                     (0x100 if role == 'builder' else 0)
            secondary = 0x40000 if role == 'monarch' else 0
            menu = 1 if role in ('builder', 'monarch') or (role == 'structure' and rng.random() < 0.5) else 0
            depth = rng.choice([5, 12, 0]) if role == 'naval' else rng.choice([-10000, -10000, 0])
            # Every mobile unit has a mover (retail reads its grid unchecked in
            # these routines); a flyer's, and now and then another's, has no grid.
            mover = int(role != 'structure')
            nav = 0 if not mover or role == 'air' or rng.random() < 0.1 else rng.randrange(1, 4)
            maxd = rng.choice([100, 1000, 4000, 0, 0xfffffff0])
            hp = rng.choice([maxd, maxd, 3 * maxd // 4, 3 * maxd // 4 - 1, maxd // 2, -1])
            hp = max(-32768, min(32767, hp))
            f108 = rng.choice([0.0] * 8 + [0.5, float('nan'), -0.0])
            held = 0 if rng.random() < 0.95 else 1
            carrier = rng.choice([0] * 12 + [1, 2])
            has = rng.random() < 0.5
            mflags = rng.choice([0x3000400, 0x80508, 0x2000, 0x8010c, 0]) if has else 0
            mclass = rng.choice([0, 1, 2, 7, 14, 19]) if has else 0
            spread = rng.choice([300, 300, 3000])
            x = ((cx + rng.randrange(-spread, spread + 1)) << 16) | rng.randrange(0x10000)
            z = ((cz + rng.randrange(-spread, spread + 1)) << 16) | rng.randrange(0x10000)
            fx, fz = rng.choice([1, 2, 3, 4]), rng.choice([1, 2, 3, 4])
            squad = 0
            if flags & 0x1000000 and not flags & 0x1000:
                if bases and rng.random() < 0.9:
                    squad = focus if rng.random() < 0.6 else rng.choice(bases)
                elif rng.random() < 0.5:
                    squad = rng.randrange(21, 99)
            if squad: members[squad].append(i)
            # Squads drop a dying unit only on the next assignment pass.
            if squad and rng.random() < 0.03: flags |= 0x1000
            put(u + 0x130, flags); uc.mem_write(u + 2, struct.pack('<H', i))
            put(u + 0x68, x); put(u + 0x70, z)
            uc.mem_write(u + 0x78, struct.pack('<hh', fx, fz))
            uc.mem_write(u + 0x10c, struct.pack('<h', hp))
            uc.mem_write(u + 0x108, struct.pack('<f', f108)); put(u + 0x104, held)
            put(u + 0xa8, [0, HOLDER, NONHOLDER][carrier])
            put(u + 0xb4, t); put(u + 0xb8, PLAYER); put(u + 0xc8, squad)
            uc.mem_write(u + 0xfd, bytes([side]))
            put(u + 8, MOVERS + i * 0x10 if mover else 0)
            if mover: put(MOVERS + i * 0x10 + 4, NAVS + (nav - 1) * 0x400 if nav else 0)
            put(t + 0x1be, maxd); put(t + 0x260, tflags); put(t + 0x264, secondary); put(t + 0x12e, menu)
            uc.mem_write(t + 0x194, struct.pack('<h', depth))
            if has:
                m = MISSIONS + i * 0x80
                put(m + 0x5a, mflags); put(u + 0x60, m)
                classes[i] = mclass
            f = struct.unpack('<I', struct.pack('<f', f108))[0]
            units.append([i, flags, s32(x), s32(z), fx, fz, hp, maxd, tflags, secondary, menu, depth, mover,
                          f, held, carrier, int(has), mflags, mclass, nav, squad])
        for s in range(1, 99):
            rng.shuffle(members[s])
            dd = DATA + s * DSTRIDE
            begin = VEC + s * CAP * 4
            for k, i in enumerate(members[s]): put(begin + 4 * k, UNITS + i * USTRIDE)
            put(dd + 0xb8, begin); put(dd + 0xbc, begin + 4 * len(members[s])); put(dd + 0xc0, begin + 4 * CAP)
        self.nunits, self.members, self.cx, self.cz = nunits, members, cx, cz

        # The visible-enemy list (build manager +5: begin +9, end +0xd).
        enemies = []
        for k in range(rng.choice([0, 1, 2, 3, 8])):
            e = ENEMIES + k * USTRIDE
            ef = rng.choice([0x1000000, 0x1000000, 0x1001000, 0])
            ex = s32((rng.randrange(0, 4000) << 16) | rng.randrange(0x10000))
            ez = s32((rng.randrange(0, 4000) << 16) | rng.randrange(0x10000))
            put(e + 0x130, ef); put(e + 0x68, ex); put(e + 0x70, ez)
            put(LIST + 4 * k, e)
            enemies.append((ef, ex, ez))
        put(BM + 9, LIST); put(BM + 0xd, LIST + 4 * len(enemies)); put(BM + 0x79, 0)
        self.enemies = enemies

        self.params, self.units = params, units

    def line(self):
        """The case's text fields (call after any change to the enemies)."""
        line = [len(self.sites)] + [v for s in self.sites for v in s] + [len(self.starts)] + [v for s in self.starts for v in s]
        for slot in range(1, 21): line += self.params[slot]
        line += [self.nunits] + [v for u in self.units for v in u]
        for s in range(1, 99): line += [len(self.members[s])] + self.members[s]
        for lists in self.navs:
            for labels in lists: line += [len(labels)] + labels
        line += [len(self.enemies)] + [v for e in self.enemies for v in e]
        return line

    def place_enemy_near(self, rng, x, z):
        """Move some listed enemies onto a point (16.16), within 0..60 px."""
        for k, (ef, ex, ez) in enumerate(self.enemies):
            if rng.random() < 0.5:
                ex = s32(x + (rng.randrange(-60, 61) << 16)); ez = s32(z + (rng.randrange(-60, 61) << 16))
                self.enemies[k] = (ef, ex, ez)
                e = ENEMIES + k * USTRIDE
                self.put(e + 0x68, ex); self.put(e + 0x70, ez)

    def ring_enemies(self, rng, points, each=2):
        """List `each` more live enemies per point (16.16), each within a 60 px box
        of its point or on a ring of 44..56 px around it (the edge of a 50 px
        query), and send the old ones far off (56 at most in all)."""
        self.enemies = [(ef, s32(0x7f000000), s32(0x7f000000)) for ef, ex, ez in self.enemies]
        for x, z in points:
            for _ in range(each):
                if rng.random() < 0.3:
                    dx, dz = rng.randrange(-60, 61), rng.randrange(-60, 61)
                else:
                    r = rng.randrange(44, 57)
                    dx = rng.randrange(-r, r + 1)
                    dz = int(round((r * r - dx * dx) ** 0.5)) + rng.choice([-1, 0, 0, 1])
                    if rng.random() < 0.5: dz = -dz
                    if rng.random() < 0.5: dx, dz = dz, dx
                self.enemies.append((0x1000000, s32(x + (dx << 16)), s32(z + (dz << 16))))
        del self.enemies[56:]
        for k, (ef, ex, ez) in enumerate(self.enemies):
            e = ENEMIES + k * USTRIDE
            self.put(e + 0x130, ef); self.put(e + 0x68, ex); self.put(e + 0x70, ez)
            self.put(LIST + 4 * k, e)
        self.put(BM + 9, LIST); self.put(BM + 0xd, LIST + 4 * len(self.enemies))

    def state(self):
        """The manager after a call: unit squads, then each squad's members and p0..p8."""
        row = [self.geti(UNITS + i * USTRIDE + 0xc8) for i in range(1, self.nunits + 1)]
        for s in range(1, 99):
            dd = DATA + s * DSTRIDE
            b, e = self.get(dd + 0xb8), self.get(dd + 0xbc)
            ids = [self.index(self.get(a)) for a in range(b, e, 4)] if b else []
            row += [len(ids)] + ids + [self.geti(dd + 0xc + 4 * k) for k in range(9)]
        return row
