#!/usr/bin/env python3
"""A water grave sinking, natively (fx-sink).

Places a body with the corpse placement 512ee0 (the real 511170 and 50e600;
the feature install 495360 is an observation boundary that hands back a
record the real allocator 4949c0 took) and then runs the whole feature
update 4959c0 once per tick, with the real 511260, 50e600 and list move
494a80; the cell extrema come from the real 50ed60. Each tick it compares
the record's height, vertical velocity, fade flag and alpha (+0x0c, +0x18,
+0x2f, +0x30), and whether it is still in the moving list or was taken off
the plane (496380), with the engine's helpers (src/sim/retailfeaturesink.h,
built into tools/re/corpse_sink_driver.cpp). Cases: random shores and sea
beds (many of height 0 or 1), the body at the ground, at sea level, under
it, and above it as a flyer would be; corpses and statues; the walls'
isfeature, which keeps no velocity.

Reclaims: in about 30% of the cases the real 494ce0 (mode 1) reclaims the
body at a random tick, before that tick's update. It must take a new record
(the free list's head), copy the position and free the old one; then the
reclaim fade's 34 passes run to 494f00 (an observation boundary) and are
compared with World's port (retailFeatureSinkReclaim: the body stops). In a
third of them the new slot still holds the sinking velocity of a record
freed while it moved; those are counted, not compared (World has no record
pool): how often the body sinks on, and how often its fade starts again.
Reloads: in about 30% of the cases, from a third stream, a reclaim wake
(406615) writes the decay +0x3c of the body's record at a random tick before
any reclaim, before that tick's update -- whether the record still moves or
not; World reloads only one still in the moving list (a settled one is
never updated again), and both count it there (retailFeatureSinkStep).
Observation only: nothing from the binary is copied into the engine.

    python3 -B tools/re/check_corpse_sink.py [--cases N] [--build DIR]
"""
import argparse, os, random, struct, subprocess
from emu import Icd, HEAP

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = HEAP; CELLS = HEAP + 0x30000; UNIT = HEAP + 0x60000; TYPE = HEAP + 0x61000
RECS = HEAP + 0x62000; FTYPES = HEAP + 0x70000
MODEL = HEAP + 0x80000; MODEL2 = HEAP + 0x81000
NREC = 4


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build-dbg', 'tmp'))
    a = ap.parse_args()
    os.makedirs(a.build, exist_ok=True)
    driver = os.path.join(a.build, 'corpse_sink_driver')
    subprocess.check_call(['g++', '-std=c++20', '-O1', '-I', os.path.join(ROOT, 'src'), '-o', driver,
                           os.path.join(ROOT, 'tools', 're', 'corpse_sink_driver.cpp')])
    ours = subprocess.Popen([driver], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    p = Icd()
    def put(addr, v, f='<I'): p.uc.mem_write(addr, struct.pack(f, v))
    def get(addr, f='<I'): return struct.unpack(f, p.uc.mem_read(addr, struct.calcsize(f)))[0]
    state = dict(record=0, removed=False, reclaimed=False)
    def install(uc, sp):
        cell, ftype, pos, _orient, _owner = struct.unpack('<5I', uc.mem_read(sp, 20))
        rec = state['record']
        uc.mem_write(rec + 8, bytes(uc.mem_read(pos, 12)))
        # A stub model instance (+4): no pieces, no animation; the decay's
        # sink into the ground (495af6..495b33) lowers its +0x1c8 object.
        uc.mem_write(rec + 4, struct.pack('<I', MODEL))
        uc.mem_write(rec + 0x2c, struct.pack('<H', ftype & 0xffff))
        index = (cell - CELLS) // 14
        uc.mem_write(rec + 0x28, struct.pack('<hh', index % W, index // W))
        # 495360's cell side: the type, the record and the object bit.
        uc.mem_write(cell + 8, struct.pack('<HH', ftype & 0xffff, (rec - RECS) // 0x60))
        uc.mem_write(cell + 0xd, bytes([uc.mem_read(cell + 0xd, 1)[0] | 8]))
        return 5, rec
    def remove(uc, sp):
        state['removed'] = True
        return 2, 1
    p.hooks[0x495360] = install
    p.hooks[0x496380] = remove
    def replaced(uc, sp):
        state['reclaimed'] = True
        return 3, 0
    p.hooks[0x494f00] = replaced
    p.hooks[0x495f30] = lambda uc, sp: (0, 0)
    p.hooks[0x502be0] = lambda uc, sp: (4, 0)
    p.freeze_hooks()
    put(0x62d55c, GAME)
    W, H = 8, 8
    put(GAME + 0x19e98, W); put(GAME + 0x19e9c, H); put(GAME + 0x19f04, CELLS)
    put(GAME + 0x19edc, FTYPES); put(GAME + 0x19e74, RECS); put(GAME + 0x19ec0, 0); put(GAME + 0x19f44, 0)
    put(UNIT + 0xb4, TYPE)
    put(MODEL, 0); put(MODEL + 0x174, 0); put(MODEL + 0x1c8, MODEL2)
    put(TYPE + 0x24e, 3, '<H'); put(TYPE + 0x250, 4, '<H'); put(TYPE + 0x252, 5, '<H')
    put(TYPE + 0x25c, 0, '<H'); put(TYPE + 0x25e, 0, '<H')

    def moving_list():
        out = []; i = get(GAME + 0x19e7c, '<i')
        while i != -1 and len(out) <= NREC:
            out.append(i); i = get(RECS + i * 0x60, '<h')
        return out

    rng = random.Random(0x4959c0); rrng = random.Random(0x494ce0); wrng = random.Random(0x406615)
    stats = dict(water=0, reloaded=0, decayed=0, removed=0, settled=0, above_sea=0, landed_fading=0, fade_settled=0, popped=0, reclaimed=0, stale=0, stale_restarted=0, stale_moved=0)
    for case in range(a.cases):
        sea = rng.choice((0, 1, 2, 10, 30, 40, 50, 120, 255))
        def pick():
            return rng.choice((0, 0, 1, max(sea - 3, 0), max(sea - 1, 0), sea, min(sea + 1, 255),
                               min(sea + 4, 255), rng.randrange(0, 90), rng.randrange(0, 256)))
        heights = [[pick() for _ in range(W)] for _ in range(H)]
        p.uc.mem_write(CELLS, bytes(14 * W * H))
        for zz in range(H):
            for xx in range(W):
                p.uc.mem_write(CELLS + (zz * W + xx) * 14 + 4, bytes([heights[zz][xx]]))
                put(CELLS + (zz * W + xx) * 14 + 8, 0xffff, '<H')
        def corners(xx, zz):
            return [heights[min(zz + dz, H - 1)][min(xx + dx, W - 1)] for dz in (0, 1) for dx in (0, 1)]
        # 50ed60 forms the extrema of the cells it is given; a whole-map call
        # stops short of the last row and column, so those are written here
        # by the same corner rule and the rest are checked against it.
        r, err = p.call(0x50ed60, (0, W | (H << 16)))
        assert not err, err
        for zz in range(H):
            for xx in range(W):
                c = CELLS + (zz * W + xx) * 14
                if xx < W - 1 and zz < H - 1:
                    assert tuple(p.uc.mem_read(c + 5, 2)) == (max(corners(xx, zz)), min(corners(xx, zz)))
                else:
                    p.uc.mem_write(c + 5, bytes([max(corners(xx, zz)), min(corners(xx, zz))]))
        p.uc.mem_write(GAME + 0x19ef8, bytes([sea]))
        gravity = rng.choice((0x1fdb, 0x1fdb, 0, 0x8000, rng.randrange(0, 0x20000)))
        put(GAME + 0x19ecc, gravity)
        # a fresh pool: every record free, linked 0..NREC-1
        p.uc.mem_write(RECS, bytes(0x60 * NREC))
        for i in range(NREC):
            put(RECS + i * 0x60, i + 1 if i + 1 < NREC else -1, '<h')
            put(RECS + i * 0x60 + 2, i - 1, '<h')
        put(GAME + 0x19e7c, -1, '<i'); put(GAME + 0x19e80, -1, '<i'); put(GAME + 0x19e84, 0, '<i')
        idx, err = p.call(0x4949c0, ())
        assert not err and idx == 0, (idx, err)
        state['record'] = RECS; state['removed'] = False; state['reclaimed'] = False
        x = rng.randrange(-8, W * 16 + 8) << 16 | rng.randrange(0, 65536)
        z = rng.randrange(-8, H * 16 + 8) << 16 | rng.randrange(0, 65536)
        if rng.random() < 0.9:
            x = rng.randrange(0, W * 16) << 16 | rng.randrange(0, 65536)
            z = rng.randrange(0, H * 16) << 16 | rng.randrange(0, 65536)
        cx, cz = int((x >> 16) / 16), int((z >> 16) / 16)
        x &= 0xffffffff; z &= 0xffffffff
        if not (0 <= cx < W and 0 <= cz < H):
            cx, cz = min(max(cx, 0), W - 1), min(max(cz, 0), H - 1)  # 512ee0's own cell must exist
        kind = rng.randrange(5)
        y = [lambda: max(heights[cz][cx], 0) << 16 | rng.randrange(0, 65536),
             lambda: sea << 16,
             lambda: rng.randrange(0, sea * 65536 + 1),
             lambda: (sea + rng.randrange(1, 200)) << 16 | rng.randrange(0, 65536),
             lambda: rng.randrange(1, 1500) * 0x2ccc][kind]()  # whole steps to a sea bed of 0
        y &= 0xffffffff
        isfeature = rng.random() < 0.05
        # A reclaim of the body (494ce0 mode 1) at a random tick, from its own
        # stream; in some, the slot it takes was last freed while it moved.
        reclaim = rrng.randrange(0, 120) if rrng.random() < 0.3 else -1
        stale = reclaim >= 0 and rrng.random() < 0.3
        # A reclaim wake's reload of +0x3c (406615) before any reclaim.
        wake, reload = -1, 0
        if wrng.random() < 0.3:
            wake = wrng.randrange(0, 150)
            reload = wrng.choice((1, 2, 30, 59, 60, 61, 90, wrng.randrange(1, 400)))
            if reclaim >= 0 and wake >= reclaim: wake = -1
        put(TYPE + 0x260, 0x1000000 if isfeature else 0)
        put(UNIT + 0x68, x); put(UNIT + 0x6c, y); put(UNIT + 0x70, z)
        put(UNIT + 0x74, cx, '<h'); put(UNIT + 0x76, cz, '<h')
        statue = rng.randrange(3)
        r, err = p.call(0x512ee0, (UNIT, 1, 0, int(statue == 1), int(statue == 2)))
        assert not err, err
        assert get(RECS + 0x2c, '<H') == (3, 4, 5)[statue]
        if get(RECS + 0x18) == 0xffffd334: stats['water'] += 1
        ticks = 3000
        if stale: put(RECS + 0x60 + 0x18, 0xffffd334)   # the next free slot (1) kept a sinking velocity
        text = '%d %d %d %d %d %d %d %d %d %d %d\n' % (W, H, sea, gravity, int(isfeature), struct.unpack('<i', struct.pack('<I', x))[0],
                                            struct.unpack('<i', struct.pack('<I', y))[0], struct.unpack('<i', struct.pack('<I', z))[0],
                                            reclaim, wake, reload)
        text += ' '.join(str(heights[zz][xx]) for zz in range(H) for xx in range(W)) + '\n%d\n' % ticks
        mine = []
        if not stale:
            ours.stdin.write(text); ours.stdin.flush()
            while True:
                line = ours.stdout.readline().strip()
                if line == 'end': break
                mine.append(line)
        theirs = []; landed_while_fading = False; index = 0; restarted = False
        for t in range(ticks):
            if t == wake: put(RECS + 0x3c, reload)
            if t == reclaim:
                if state['removed']: break
                cell = CELLS + (get(RECS + 0x2a, '<h') * W + get(RECS + 0x28, '<h')) * 14
                r, err = p.call(0x494ce0, (get(RECS + 0x28, '<h'), get(RECS + 0x2a, '<h'), 1))
                assert not err, err
                index = get(cell + 0xa, '<H')
                assert index == 1 and get(GAME + 0x19e84, '<i') == 0, 'a new record, the old one freed'
                assert get(RECS + 0x60 + 0xc, '<i') == get(RECS + 0xc, '<i') and get(RECS + 0x60 + 0x2e, '<B') == 1
            r, err = p.call(0x4959c0, ())
            assert not err, err
            rec = RECS + index * 0x60
            if state['removed']: st = 2
            elif state['reclaimed']: st = 3
            elif index == 0 and 0 not in moving_list(): st = 1
            else: st = 0
            theirs.append('%d %d %d %u %d' % (get(rec + 0xc, '<i'), get(rec + 0x18, '<i'), get(rec + 0x2f, '<B'),
                                              get(rec + 0x30), st))
            if index and get(rec + 0x2f, '<B'): restarted = True
            if st == 0 and get(rec + 0x2f, '<B') and get(rec + 0x18) == 0: landed_while_fading = True
            if st == 2 or st == 3 or (st == 1 and reclaim < 0): break
        if stale:
            # Not ported (World has no record pool): the body sinks on from
            # the copied position, and over a bed of 0 its fade starts again
            # from alpha 1.0 on top of the reclaim's.
            stats['stale'] += 1; stats['stale_restarted'] += restarted
            stats['stale_moved'] += reclaim < len(theirs) - 1 and theirs[reclaim].split()[0] != theirs[-1].split()[0]
            continue
        if reclaim >= 0: stats['reclaimed'] += 1
        if wake >= 0 and wake < len(theirs): stats['reloaded'] += 1
        stats['decayed'] += wake >= 0 and theirs[-1].split()[-1] == '2' and not any(
            line.split()[2] == '1' for line in theirs)
        if theirs != mine:
            for i, (u, v) in enumerate(zip(theirs, mine)):
                if u != v: print('first difference at tick', i, 'retail', u, 'ours', v); break
            raise SystemExit('MISMATCH case %d: sea %d gravity %#x x %#x y %#x z %#x (%d vs %d ticks)'
                             % (case, sea, gravity, x, y, z, len(theirs), len(mine)))
        last = int(theirs[-1].split()[-1])
        stats['removed'] += last == 2; stats['settled'] += last == 1
        stats['above_sea'] += kind == 3
        stats['landed_fading'] += landed_while_fading
        faded = any(line.split()[2] == '1' for line in theirs)
        stats['fade_settled'] += faded and last == 1
        stats['popped'] += int(theirs[0].split()[0]) > y if y < 0x80000000 else 0
    ours.stdin.close(); ours.wait()
    print('ok', a.cases, 'cases;', ', '.join('%s %d' % kv for kv in stats.items()))


if __name__ == '__main__':
    main()
