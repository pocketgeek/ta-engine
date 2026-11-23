"""Set TotalA.exe's route (path_model.search, verified against the binary by
path_emu.py) beside the engine's Retail kernel (Kingdoms port, driven by
path_kingdoms_cmp.cpp) on identical grade planes.  Observe only.

Usage: path_compare.py <kcmp binary> [cases] [seed] [turnrate]
"""
import sys, random, subprocess, math
sys.path.insert(0, '/home/pocket_geek/TA/tools/re')
import path_model as M


def length(route):
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(route, route[1:]))


def main():
    exe = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 11)
    turn = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    cases, lines = [], []
    for _ in range(n):
        w, h = 2 * rng.randint(8, 24), 2 * rng.randint(8, 24)   # TA maps are whole 32px tiles
        m = M.Map.random(rng, w, h, density=rng.choice([0.05, 0.15, 0.3]), noise=rng.choice([1, 3, 5]))
        m.hidden.clear()
        f = rng.choice([1, 2, 3])
        cls = M.MoveClass(f, f, maxslope=10, badslope=5, maxwslope=10, badwslope=5)
        plane = M.class_plane(m, cls)
        start = (rng.randrange(w), rng.randrange(h)); goal = (rng.randrange(w), rng.randrange(h))
        if plane[start[1]][start[0]] == 0: continue
        tol = 4
        heading = rng.randrange(65536)
        weight = 0x18000
        ta = M.search(m, cls, plane, start, goal, tol, heading, weight)
        rc = int(tol / 16)
        lines.append(f'{w} {h} {f} {tol} {rc * rc} {heading} {weight} {turn} {start[0]} {start[1]} {goal[0]} {goal[1]} '
                     + ' '.join(str(plane[z][x]) for z in range(h) for x in range(w)))
        cases.append((ta, f, start, goal))
    out = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True).stdout.splitlines()
    same = 0; outcome = {}; ratios = []; wps = [0, 0]; nboth = 0
    for (ta, f, start, goal), line in zip(cases, out):
        parts = line.split()
        note, flags = int(parts[0]), int(parts[1])
        cells = [tuple(map(int, p.split(','))) for p in parts[4:]]
        eng = [((x * 2 + f) * 8, (z * 2 + f) * 8) for x, z in cells]
        ek = ('reached' if note == 0x1000 and eng and not flags & 4 else
              'partial' if eng else 'none')
        tk = {'reached': 'reached', 'partial': 'partial'}.get(ta['kind'], 'none')
        outcome[(tk, ek)] = outcome.get((tk, ek), 0) + 1
        if eng == ta['route']: same += 1
        elif '-v' in sys.argv and (tk, ek) != ('partial', 'partial'):
            print('  diff', tk, ek, 'start', start, 'goal', goal, 'retry', parts[3], 'flags', flags, 'work', parts[2])
            print('     TA ', ta['route']); print('     eng', eng)
        if tk == ek == 'reached' and ta['route'] and len(eng) > 1:
            nboth += 1
            ratios.append(length(eng) / max(1e-9, length(ta['route'])))
            wps[0] += len(ta['route']); wps[1] += len(eng)
    print(f'turnrate {turn}: {len(cases)} cases, identical routes {same}')
    print('  outcome (TotalA.exe, engine):', dict(sorted(outcome.items())))
    if ratios:
        ratios.sort()
        print(f'  both reached {nboth}: engine/TA path length median {ratios[len(ratios)//2]:.3f} '
              f'min {ratios[0]:.3f} max {ratios[-1]:.3f}; mean points TA {wps[0]/nboth:.1f} engine {wps[1]/nboth:.1f}')


if __name__ == '__main__':
    main()
