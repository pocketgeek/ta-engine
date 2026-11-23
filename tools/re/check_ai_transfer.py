#!/usr/bin/env python3
"""Compare the Classic brain's base-to-base transfers with retail's.

Per case it runs, for one base squad of a random AI manager
(classic_base_env.py: up to 40 units over a few bases, anchored on start or
sacred-site records or not, their movers' navigator grids with zone labels per
record, some grids with a label too many or too few), retail's three transfer
targets 0x40b050 (a base short of builders against its p1), 0x40b140 (ground
units against p2) and 0x40b230 (air units against p3), and then the transfer
0x40b9e0(to, n, category, clear) for a random destination (often one the
searches found), count, category and clear flag. Everything is retail's own
code -- the ready count 0x40a7b0, the base centre 0x40c610 with its anchors
and centroid, the categories 0x40a5f0, the 64-bit squares 0x5d3c60, set-squad
0x50b4f0 and the vector code -- except operator new and delete and the
mission clear 0x4d6a50, which is logged.

The port (classic_ai_test --transfer: ai::classicShortBase and
ai::classicTransfer) must give the same three bases, make the same draws and
clears in the same order and leave the same seed, unit squads, member lists
and squad parameters.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_transfer.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import subprocess

from emu import Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW
from classic_base_env import Env, SQOBJ, s32


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
    for address, name in ((0x40b111, 'builder target'), (0x40b201, 'ground target'), (0x40b2f1, 'air target'),
                          (0x40baaf, 'drawn'), (0x40bbc1, 'other zone'), (0x40bbc7, 'moved'),
                          (0x40bbce, 'cleared'), (0x40bbf6, 'label table mismatch')):
        uc.hook_add(UC_HOOK_CODE, count(name), begin=address, end=address)

    rng = random.Random(0x40b9e0)
    rows, expected = [], []
    for n in range(args.cases):
        slot = rng.choice([1, 1, 2, 3, rng.randrange(1, 21)])
        env.build(rng, focus=slot)
        obj = SQOBJ + slot * 0x20
        uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        targets = []
        for routine in (0x40b050, 0x40b140, 0x40b230):
            r, error = p.call(routine, ecx=obj)
            if error: raise RuntimeError(error)
            targets.append(s32(r))
        found = [t for t in targets if t]
        to = rng.choice(found + found + [rng.randrange(1, 21), slot]) if found else rng.randrange(1, 21)
        count_ = rng.choice([0, 1, 1, 1, 2, 3, 6, -1])
        category = rng.choice([2, 2, 4, 4, 5, 3, 6])
        clear = rng.randrange(2)
        seed = rng.getrandbits(31) | 1
        env.put(0x64186c, seed)
        env.log.clear()
        _, error = p.call(0x40b9e0, (to, count_, category, clear), ecx=obj)
        if error: raise RuntimeError(error)
        expected.append(targets + [env.get(0x64186c), len(env.log)] + [v for e in env.log for v in e] + env.state())
        rows.append(' '.join(map(str, [seed, slot, to, count_, category, clear] + env.line())))

    out = subprocess.run([args.binary, '--transfer'], input='\n'.join(rows) + '\n',
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
    print('PASS: %d 0x40b050/0x40b140/0x40b230 searches + 0x40b9e0 transfers, 0 mismatches; retail paths reached: %s'
          % (len(rows), ', '.join('%s %d' % kv for kv in sorted(hits.items()))))


if __name__ == '__main__':
    main()
