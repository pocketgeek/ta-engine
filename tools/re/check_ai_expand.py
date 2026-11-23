#!/usr/bin/env python3
"""Compare the Classic brain's expansion with retail's.

Per case it runs retail's expansion 0x40bc20(newSlot, escort) for one base
squad of a random AI manager (classic_base_env.py: up to 40 units over a few
bases anchored on start or sacred-site records or not, free and busy builders,
their movers' navigator grids with zone labels per record -- some grids with a
label too many or too few, flyers without one), a destination slot (usually
the first empty base) and an escort size, with the visible-enemy list's units
now and then put within 60 px of a sacred site, or near the edge of the 50 px
query around one. Everything is retail's own code -- the founder filter and
categories 0x40a5f0, the vector code, the base centre 0x40c610 and ring-2
radius 0x40cf30 of every occupied base, the 50 px of 0x40cfa0 / 0x40cfc0, the
enemy query 0x411ae0, set-squad 0x50b4f0 and the anchor setter 0x40c6e0 --
except operator new and delete.

The port (classic_ai_test --expand: ai::classicExpand) must give the same
answer, make the same draws and leave the same seed, unit squads, member lists
and squad parameters (the new base's anchor among them).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_expand.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import subprocess

from emu import Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW
from classic_base_env import Env, SQOBJ, DATA, DSTRIDE, s32


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=4000)
    args = ap.parse_args()
    p = Icd()
    env = Env(p)
    p.hooks.update(env.hooks())
    p.freeze_hooks()
    uc = p.uc
    uc.hook_add(UC_HOOK_CODE, lambda uc_, a, s, d: env.log.append(
        (0, s32(env.get(uc_.reg_read(UC_X86_REG_ESP) + 4)))), begin=0x535cc0, end=0x535cc0)
    hits = {}
    def count(name):
        def hook(uc_, a, size, d): hits[name] = hits.get(name, 0) + 1
        return hook
    for address, name in ((0x40bd0c, 'builders tried'), (0x40bdc9, 'no founder'), (0x40bdf5, 'flyer founder'),
                          (0x40be0b, 'unanchored'), (0x40be5f, 'site labels mismatch'),
                          (0x40beb3, 'start labels mismatch'), (0x40bf1a, 'no centre'),
                          (0x40c062, 'site anchored'), (0x40c0d3, 'base checked'), (0x40c0e5, 'site kept'),
                          (0x40c122, 'no site'), (0x40c18b, 'refused (enemies)'), (0x40c1be, 'founded')):
        uc.hook_add(UC_HOOK_CODE, count(name), begin=address, end=address)

    rng = random.Random(0x40bc20)
    rows, expected = [], []
    for n in range(args.cases):
        slot = rng.choice([1, 1, 1, 2, 3, rng.randrange(1, 21)])
        env.build(rng, focus=slot)
        if rng.random() < 0.5:
            for valid, x, z in env.sites:
                if env.enemies and rng.random() < 0.7:
                    env.place_enemy_near(rng, (x << 20) + 0x80000, (z << 20) + 0x80000)
        else:
            # Enemies around every site, most on the edge of the 50 px query.
            env.ring_enemies(rng, [((x << 20) + 0x80000, (z << 20) + 0x80000) for valid, x, z in env.sites if valid],
                             rng.choice([1, 1, 2, 3]))
        empty = [b for b in range(2, 21) if not env.members[b]]
        to = (empty[0] if rng.random() < 0.8 else rng.choice(empty)) if empty else rng.randrange(2, 21)
        escort = rng.choice([0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 40])
        seed = rng.getrandbits(31) | 1
        env.put(0x64186c, seed)
        env.log.clear()
        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        r, error = p.call(0x40bc20, (to, escort), ecx=SQOBJ + slot * 0x20)
        if error: raise RuntimeError(error)
        expected.append([r & 0xff, env.get(0x64186c), len(env.log)] + [v for e in env.log for v in e] + env.state())
        rows.append(' '.join(map(str, [seed, slot, to, escort] + env.line())))

    out = subprocess.run([args.binary, '--expand'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if bad:
        i = bad[0]
        diff = [(k, a, b) for k, (a, b) in enumerate(zip(actual[i], expected[i])) if a != b][:6]
        raise AssertionError('%d of %d cases differ; first: case %d, (index, port, retail) %s\n port   %s\n retail %s\n%s'
                             % (len(bad), len(rows), i, diff, actual[i][:60], expected[i][:60], rows[i][:1500]))
    print('PASS: %d 0x40bc20 expansions, 0 mismatches; retail paths reached: %s'
          % (len(rows), ', '.join('%s %d' % kv for kv in sorted(hits.items()))))


if __name__ == '__main__':
    main()
