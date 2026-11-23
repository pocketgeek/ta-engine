#!/usr/bin/env python3
"""Compare World::retailGuardNoMoveSlots with retail's Guard_NoMove armed
step 0x401a90(unit).

Random cases over: one to three present slots, a switcher and its selected
slot, the per-slot shot bits +0xd0 & 0xe00000 (0x200000 << slot), the rand(3)
answer, and per slot its automatic flag (slot flags +0x1a bit 4) and its
target: none, inside the minimum range (0x52adf0), out of reach (!0x52ad60)
or good. Retail runs the real 0x401a90 and the real slot setters 0x519b70 /
0x519b10; hooked: the slot target getter 0x51a9a0, 0x52adf0 and 0x52ad60
(scripted per slot), the target clear 0x51a7f0 (logged) and rand 0x535cc0
(scripted, logged). The port (fire_sweep_test --guard) builds a three-slot
tower with a real enemy per slot at a distance that gives the same answer
(range 300, minimum range 100), then runs retailGuardNoMoveSlots with the
same draw. Compared: each slot's final automatic flag, whether its target
was cleared, and the slot bits left; and the port draws exactly when retail
does (a bit set).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_guard_nomove.py --binary build-o2/fire_sweep_test
"""
import argparse
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

U, D = HEAP + 0x1000, HEAP + 0x2000
SLOT = lambda i: U + 0xc + 0x1c * i
NONE, INSIDE, OUT, GOOD = range(4)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/fire_sweep_test')
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=401)
    args = ap.parse_args()
    p = Icd(); uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    get16 = lambda a: struct.unpack('<H', uc.mem_read(a, 2))[0]
    state, log = {}, []

    def slot_arg(sp, n):
        return struct.unpack(f'<{n}I', uc.mem_read(sp, 4 * n))[-1] & 0xff

    def target(uc_, sp):
        return 2, 0x7000 + slot_arg(sp, 2) if state['slots'][slot_arg(sp, 2)] != NONE else 0

    def inside(uc_, sp):
        return 3, int(state['slots'][slot_arg(sp, 3)] == INSIDE)

    def inrange(uc_, sp):
        return 3, int(state['slots'][slot_arg(sp, 3)] not in (OUT,))

    def clear(uc_, sp):
        log.append(('clear', slot_arg(sp, 2))); return 2, 0

    def rand(uc_, sp):
        n = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        log.append(('rand', n)); return 1, state['draw']

    p.hooks.update({0x51a9a0: target, 0x52adf0: inside, 0x52ad60: inrange,
                    0x51a7f0: clear, 0x535cc0: rand})
    p.freeze_hooks()
    rng = random.Random(args.seed)
    rows, expected, draws = [], [], []
    for _ in range(args.cases):
        n = rng.randint(1, 3)
        switcher = rng.random() < 0.3
        selected = rng.randrange(n)
        bits = sum(0x200000 << s for s in range(3) if rng.random() < 0.3)
        draw = rng.randrange(3)
        auto = [rng.randrange(2) for _ in range(n)]
        slots = [rng.randrange(4) for _ in range(n)]
        uc.mem_write(U, bytes(0x138)); uc.mem_write(D, bytes(0x300)); put(U + 0xb4, D)
        put(U + 0x130, 0x1000000 | 0x8000000 | (selected << 30))
        if switcher: put(D + 0x260, 0x10000)
        for i in range(3):
            put(SLOT(i), 0x5000 + i if i < n else 0)
            put16(SLOT(i) + 0x1a, 4 if i < n and auto[i] else 0)
        put(U + 0xd0, bits | 0x1)
        state.update(slots=slots + [NONE] * (3 - n), draw=draw)
        log.clear()
        r, err = p.call(0x401a90, (U,)); assert err is None, err
        parts = []
        for i in range(n):
            cleared = slots[i] != NONE and ('clear', i) in log
            parts.append(f'{int(bool(get16(SLOT(i) + 0x1a) & 4))}:{int(cleared)}')
        parts.append(str(get(U + 0xd0) & 0xe00000))
        expected.append(' '.join(parts))
        drew = sum(1 for x in log if x[0] == 'rand')
        draws.append((drew, bits))
        rows.append(f'{n} {int(switcher)} {selected} {bits} {draw} ' +
                    ' '.join(f'{auto[i]} {slots[i]}' for i in range(n)))
    bad_draws = [(row, d) for row, d in zip(rows, draws) if d[0] != (1 if d[1] else 0)]
    for row, d in bad_draws:
        print(f'FAIL retail draw count {d[0]} with bits {d[1]:#x}: {row}')
    proc = subprocess.run([args.binary, '--guard'], input='\n'.join(rows) + '\n', text=True,
                          capture_output=True, check=True)
    actual = [l.strip() for l in proc.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError(f'{len(actual)} answers for {len(expected)} cases')
    bad = [(rows[i], want, got) for i, (want, got) in enumerate(zip(expected, actual)) if want != got]
    for row, want, got in bad[:40]:
        print(f'FAIL slots switcher selected bits draw [auto state]* = {row}\n  retail {want}\n  port   {got}')
    released = sum(1 for e in expected if any(t.endswith(':1') for t in e.split()[:-1]))
    ok = not bad and not bad_draws
    print(f'{"PASS" if ok else "FAIL"}: {len(rows) - len(bad)}/{len(rows)} 401a90 calls ({released} clear a '
          f'target, {sum(1 for d in draws if d[0])} draw) match retail')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
