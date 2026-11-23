"""Independent Python reading of TotalA.exe's ground pathfinding (observe only).

Written from the disassembly notes in docs/retail-pathfinding.md, not from
binary bytes; path_emu.py runs the retail routines under unicorn and checks
this model against them cell for cell and route for route.
"""
import random

DX = [0, -1, -1, -1, 0, 1, 1, 1]          # 0x4fd670 (observed values)
DZ = [-1, -1, 0, 1, 1, 1, 0, -1]          # 0x4fd678
STEP = [16, 22, 16, 22, 16, 22, 16, 22]   # set at 0x40e66b / 0x40d880
TURN = [0, 40, 60, 80, 100, 80, 60, 40]   # 0x4fca10 (observed values)
SHORT_TURN, MIN_STRAIGHT, BAD_ENTRY = 75, 5, 30


class MoveClass:
    def __init__(self, fx=1, fz=1, maxdepth=10000, mindepth=-10000, maxslope=255, badslope=None,
                 maxwslope=255, badwslope=None, stamp=970):
        self.fx, self.fz, self.maxdepth, self.mindepth = fx, fz, maxdepth, mindepth
        self.maxslope = maxslope
        self.badslope = maxslope // 2 if badslope is None else badslope
        self.maxwslope = maxwslope
        self.badwslope = maxwslope // 2 if badwslope is None else badwslope
        self.stamp = stamp

    @staticmethod
    def random(rng, foot=(1, 1)):
        ms = rng.choice([6, 10, 16, 32, 255]); mws = rng.choice([ms, 255, 12])
        ms = min(ms, mws)                                   # 0x4403fa clamp
        bs = min(rng.choice([ms // 2, ms // 3, ms]), ms)
        bws = min(rng.choice([mws // 2, mws]), mws)
        md = rng.choice([10000, 20, 0, -5]); mn = rng.choice([-10000, 10, 0])
        return MoveClass(foot[0], foot[1], md, mn, ms, bs, mws, bws)

    def __repr__(self):
        return (f'cls(f={self.fx}x{self.fz} depth {self.mindepth}..{self.maxdepth} '
                f'slope {self.badslope}/{self.maxslope} water {self.badwslope}/{self.maxwslope})')


class Map:
    def __init__(self, w, h, height, sea=40, tick=1000):
        self.w, self.h, self.height, self.sea, self.tick = w, h, height, sea, tick
        self.feature = [[0xffff] * w for _ in range(h)]
        self.unit = [[0] * w for _ in range(h)]
        self.units = []        # (has mover, stamp) for unit index 1..
        self.hidden = set()    # unexplored 32px cells

    def lohi(self, x, z):
        # 0x483297: min/max over the cell's own height and its +x, +z, +xz
        # neighbours, each clipped at the map edge.
        hs = [self.height[z][x]]
        if x + 1 < self.w: hs.append(self.height[z][x + 1])
        if z + 1 < self.h: hs.append(self.height[z + 1][x])
        if x + 1 < self.w and z + 1 < self.h: hs.append(self.height[z + 1][x + 1])
        return min(hs), max(hs)

    def visible_bits(self, vx, vz):
        return 0 if (vx, vz) in self.hidden else 0xffff

    @staticmethod
    def random(rng, w, h, density=0.2, obstacles=True, noise=12):
        base = rng.randint(20, 70)
        height = [[max(0, min(255, base + rng.randint(-noise, noise))) for _ in range(w)] for _ in range(h)]
        m = Map(w, h, height, sea=rng.choice([0, 40, 50]))
        if obstacles:
            for z in range(h):
                for x in range(w):
                    r = rng.random()
                    if r < density * 0.4: m.feature[z][x] = 0          # blocking feature
                    elif r < density * 0.5: m.feature[z][x] = 1        # non-blocking feature
                    elif r < density * 0.7:
                        m.units.append(rng.choice([(1, 900), (1, 990), (0, 0), (1, 1000)]))
                        m.unit[z][x] = len(m.units)
                    elif r < density: m.height[z][x] = 255             # cliff spike
            for _ in range(rng.randint(0, 3)):
                m.hidden.add((rng.randrange(max(1, w // 2)), rng.randrange(max(1, h // 2))))
        return m


def raw_grade(m, cls, x, z):
    """0x47de60: 0 blocked, 1 passable at a penalty, 3 clear."""
    f = m.feature[z][x]
    if f != 0xffff:
        blocking = f >= 2 or f == 0   # feature 0 is the blocking definition in the harness
        if f >= 0xfffb: blocking = f != 0xfffe   # (continuation cells not used here)
        if blocking: return 0
    u = m.unit[z][x]
    if u:
        hasmover, stamp = m.units[u - 1]
        if not hasmover or stamp < cls.stamp: return 0
    lo, hi = m.lohi(x, z)
    if lo < m.sea - cls.maxdepth: return 0
    if hi > m.sea - cls.mindepth: return 0
    slope = (hi - lo) & 0xff
    if lo < m.sea:
        return 3 if slope <= cls.badwslope else (1 if slope <= cls.maxwslope else 0)
    return 3 if slope <= cls.badslope else (1 if slope <= cls.maxslope else 0)


def _pass(vals, n, foot):
    """One axis of 0x440500: window minimum over [i, i+foot) with zero padding
    outside, demoted from 3 to 1 when either neighbour just outside is below 3."""
    pad = lambda i: vals[i] if 0 <= i < n else 0
    out = []
    for i in range(n):
        g = min(pad(i + k) for k in range(foot))
        if g == 3 and (pad(i - 1) < 3 or pad(i + foot) < 3): g = 1
        out.append(g)
    return out


def class_plane(m, cls):
    raw = [[raw_grade(m, cls, x, z) for x in range(m.w)] for z in range(m.h)]
    rows = [_pass(raw[z], m.w, cls.fx) for z in range(m.h)]
    cols = [_pass([rows[z][x] for z in range(m.h)], m.h, cls.fz) for x in range(m.w)]
    return [[cols[x][z] for x in range(m.w)] for z in range(m.h)]


def query(m, cls, plane, x, z, player=0):
    """0x40d7b0: bounds, then 32px visibility, then the cached grade."""
    if not (0 <= x < m.w and 0 <= z < m.h): return 0
    vx, vz = (x >> 1) + (cls.fx >> 2), (z >> 1) + (cls.fz >> 2)
    if vx >= m.w >> 1 or vz >= m.h >> 1: return 0
    if not (m.visible_bits(vx, vz) >> player) & 1: return 2
    return plane[z][x]


def circle_distance(dx, dz, tol):
    ax, az = abs(dx), abs(dz)
    d = 18 * ax + 7 * az if ax > az else 7 * ax + 18 * az
    return 0 if d < tol else d - tol


def search(m, cls, plane, start, goal, radius_px, heading, weight):
    W, H = m.w, m.h
    q = lambda x, z: query(m, cls, plane, x, z)
    tol = radius_px
    rc = int(radius_px / 16)
    r2 = rc * rc
    wdist = lambda x, z: (circle_distance(x - goal[0], z - goal[1], tol) * weight) >> 16
    flags, dirs, node = {}, {}, {}
    F = lambda c: flags.get(c, 0)
    # 0x40e6c4: mark controller goal cells, choose the nearest by squared distance
    if 0 <= goal[0] < W and 0 <= goal[1] < H: flags[goal] = 4
    best_goal = goal
    res = dict(route=[], notify=0, radius=0, kind='')
    if (start[0] - goal[0]) ** 2 + (start[1] - goal[1]) ** 2 <= r2:      # 0x40e749
        res.update(notify=0x100, kind='at-goal'); return res
    h0 = wdist(*start)
    if not (0 <= start[0] < W and 0 <= start[1] < H):
        res.update(notify=0x200, kind='outside'); return res

    # -------- tracer 0x40e160
    def tracer():
        best = h0
        if q(*start) < 1: return best
        cx, cz = start
        while True:
            if best == 0: return 0
            gx, gz = best_goal
            d = 2 if gx - cx < 0 else 6 if gx - cx > 0 else (0 if gz - cz <= 0 else 4)
            nx, nz = cx + DX[d], cz + DZ[d]
            if q(nx, nz) >= 1:
                c = (nx, nz); dirs[c] = d; flags[c] = F(c) | 8
                if F(c) & 4: return 0
                best = min(best, wdist(nx, nz)); cx, cz = nx, nz
                continue
            ox, oz = cx, cz
            a = b_ = (cx, cz)
            da = db = (d + 2) & 7
            moved = False
            def online(p):
                gx_, gz_ = best_goal[0] - ox, best_goal[1] - oz
                px, pz = p[0] - ox, p[1] - oz
                if gx_ < 0: gx_, px = -gx_, -px
                if gz_ < 0: gz_, pz = -gz_, -pz
                return (pz == 0 and 0 < px <= gx_) or (px == gx_ and 0 < pz <= gz_)
            left = False
            while not left:
                enda = (da - 3) & 7; da = (da - 2) & 7
                while True:
                    n = (a[0] + DX[da], a[1] + DZ[da])
                    if q(*n) >= 1: break
                    if da == enda: return best
                    da = (da + 1) & 7
                if a == b_ and da == db and moved: return best
                a = n; moved = True; dirs[a] = da; flags[a] = F(a) | 8
                if F(a) & 4: return 0
                if online(a): cx, cz = a; left = True; break
                best = min(best, wdist(*a))
                endb = (db + 3) & 7; db = (db + 2) & 7
                while True:
                    n = (b_[0] - DX[db], b_[1] - DZ[db])
                    if q(*n) >= 1: break
                    if db == endb: return best
                    db = (db - 1) & 7
                if b_ == a and da == db: return best
                b_ = n; dirs[b_] = db; flags[b_] = F(b_) | 8
                if F(b_) & 4: return 0
                if online(b_): cx, cz = b_; left = True; break
                best = min(best, wdist(*b_))
    gr = tracer()
    res['radius'] = gr
    if gr != 0:
        res['notify'] = 0x200
        if gr >= h0:
            res['kind'] = 'unreachable'; return res
        res['kind'] = 'partial'
    else:
        res['notify'] = 0x100; res['kind'] = 'reached'

    # -------- A* 0x40e7fc.. / 0x40eda.. / 0x40da70
    heap = []        # list of node dicts
    def up(i):
        n = heap[i]
        while i > 0:
            p = (i - 1) >> 1
            if n['f'] >= heap[p]['f']: break
            heap[i] = heap[p]; heap[i]['i'] = i; i = p
        heap[i] = n; n['i'] = i
    def down(i):
        n = heap[i]
        while True:
            l, r = 2 * i + 1, 2 * i + 2
            if r < len(heap):
                c = r if heap[r]['f'] < heap[l]['f'] else l
            elif l < len(heap): c = l
            else: break
            if heap[c]['f'] >= n['f']: break
            heap[i] = heap[c]; heap[i]['i'] = i; i = c
        heap[i] = n; n['i'] = i
    def remove(n):
        i = n['i']; last = heap.pop()
        if i < len(heap):
            heap[i] = last; last['i'] = i; down(i)
    flags[start] = F(start) | 1
    dirs[start] = ((heading + 0x1000) >> 13) & 7
    sn = dict(cell=start, g=0, f=h0, entry=0, straight=100, i=0)
    heap.append(sn); node[start] = sn
    pending = False
    spread = 4
    while True:
        if len(heap) - pending == 0:
            res['route'] = []; res['kind'] += '+exhausted'; return res
        if pending:
            pending = False; remove(heap[0])
        root = heap[0]; parent = dict(root); pending = True
        c = root['cell']
        if F(c) & 4:
            end = c; break
        flags[c] = 2
        for rel in range(-spread, spread + 1):
            r7 = rel & 7
            d = (dirs[c] + r7) & 7
            nc = (c[0] + DX[d], c[1] + DZ[d])
            if not (0 <= nc[0] < W and 0 <= nc[1] < H): continue
            st = F(nc) & 3
            if st >= 2: continue
            if st == 1:
                n = node[nc]
                cost = TURN[r7] + STEP[d] + parent['g'] + n['entry']
                if r7 and parent['straight'] < MIN_STRAIGHT: cost += SHORT_TURN
                if cost >= n['g']: continue
                dirs[nc] = d
                n['f'] += cost - n['g']; n['g'] = cost
                n['straight'] = 1 if r7 else parent['straight'] + 1
                if pending:
                    old = heap[0]; up(n['i'])
                    if old['i'] != 0:
                        pending = False; remove(old)
                else:
                    up(n['i'])
                continue
            g = q(*nc)
            if g < 1 and not (F(nc) & 8):
                flags[nc] = F(nc) | 3; continue
            hh = wdist(*nc)
            flags[nc] = F(nc) | (5 if hh <= gr else 1)
            dirs[nc] = d
            straight = 1 if r7 else parent['straight'] + 1
            entry = 0 if g > 1 else BAD_ENTRY
            cost = TURN[r7] + STEP[d] + parent['g'] + entry
            if r7 and parent['straight'] < MIN_STRAIGHT: cost += SHORT_TURN
            nn = dict(cell=nc, g=cost, f=cost + hh, entry=entry, straight=straight)
            if pending:
                heap[0] = nn; nn['i'] = 0; down(0); pending = False
            else:
                heap.append(nn); up(len(heap) - 1)
            node[nc] = nn
        spread = 2
    # -------- reconstruction 0x40e050
    ring = [None] * 64
    ring[0] = end; n = 1; d = dirs[end]; cur = end
    while cur != start:
        cd = dirs[cur]
        if cd != d:
            ring[n & 63] = cur; n += 1; d = cd
        cur = (cur[0] - DX[d], cur[1] - DZ[d])
    ring[n & 63] = start; n += 1
    cnt = min(n, 64)
    cells = [ring[(n - 1 - i) & 63] for i in range(cnt)]
    res['route'] = [((x * 2 + cls.fx) * 8, (z * 2 + cls.fz) * 8) for x, z in cells]
    return res


def rect_grade(m, cls, x, z, w, h):
    """0x47dfc0: minimum raw grade over a rectangle; 0 when it leaves the map
    or touches its LAST row/column (x+w >= width, z+h >= height)."""
    if x < 0 or z < 0 or x + w >= m.w or z + h >= m.h: return 0
    return min(raw_grade(m, cls, x + i, z + j) for j in range(h) for i in range(w))


def refresh_grade(m, cls, x, z):
    """0x47e1f0: the per-cell re-rate behind 0x440830 (local refresh)."""
    fx, fz = cls.fx, cls.fz
    g = rect_grade(m, cls, x, z, fx, fz)
    if g <= 1: return g
    for sx, sz, sw, sh in ((x - 1, z - 1, fx + 1, 1), (x + fx, z - 1, 1, fz + 1),
                           (x, z + fz, fx + 1, 1), (x - 1, z, 1, fz + 1)):
        if rect_grade(m, cls, sx, sz, sw, sh) != 3: return 1
    return 3
