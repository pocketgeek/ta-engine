#!/usr/bin/env python3
"""Compare World::retailMissionAttack with retail's attack-issuing gate
0x4d7de0(unit, target, explicit, point, auto, forceLeash).

Every combination of mover or not, stance (Hold Position, Maneuver, Roam),
fire state (Hold Fire, Return Fire, Fire At Will) and call (explicit; auto;
auto with the forced leash), each with the point its callers pass: the
unit's own position (the ground missions Standby 0x40784e, Patrol 0x403972
and the hit response 0x409e4b) or none (the VTOL missions, 0x4176cc and
the rest). Retail runs the real gate; hooked: the resolver 0x4de530 (it
answers a mission type), operator new 0x4eb9e0, the mission ctor 0x4d6c40
(logged: kind, mode +0x52, leash +0x56, the Move's goal), the insert
0x4d7750, dtor/free. After the call the attack's origin (+0x2e/+0x30) is
read back. The port (fire_sweep_test --gate) runs retailMissionAttack on the
same unit: a walker for a mover with a point, a flyer for a mover without
one, a building for a non-mover; it must answer the same result and the
same missions in the same order, with the same mode, leash, origin and
return goal.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_issue_gate.py --binary build-o2/fire_sweep_test
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

U, T, D, MOVER = HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000
ATTACK, RETURN = 9, 14
UX, UZ, LEASH = 500, 600, 333
STANCE = (0, 0x10000, 0x20000)          # Hold Position, Maneuver, Roam (+0x130 bits 16-17)
FIRE = (0, 0x40000, 0x80000)            # Hold Fire, Return Fire, Fire At Will (bits 18-19)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/fire_sweep_test')
    args = ap.parse_args()
    p = Icd(); uc = p.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
    get16 = lambda a: struct.unpack('<h', uc.mem_read(a, 2))[0]
    bump = [HEAP + 0x100000]
    log = []

    def new(uc_, sp):
        bump[0] += 0x100
        uc.mem_write(bump[0], bytes(0x100))
        return 0, bump[0]

    def resolver(uc_, sp):
        out, kind = struct.unpack('<2I', uc.mem_read(sp, 8))
        uc.mem_write(out, bytes(({3: ATTACK, 2: RETURN}.get(kind, 0),))); return 6, out

    def ctor(uc_, sp):            # 4d6c40(kind, target, point, ?, mode, leash, 2 x 12-byte vectors)
        this = uc.reg_read(UC_X86_REG_ECX)
        a = struct.unpack('<6I', uc.mem_read(sp, 24))
        uc.mem_write(this + 4, bytes((a[0] & 0xff,)))
        goal = (get16(a[2] + 2), get16(a[2] + 0xa)) if a[2] else None
        log.append({'kind': a[0] & 0xff, 'mode': a[4], 'leash': a[5], 'goal': goal, 'this': this})
        return 12, this

    p.hooks.update({0x4eb9e0: new, 0x4de530: resolver, 0x4d6c40: ctor, 0x4d7750: lambda u, s: (2, 0),
                    0x4d6da0: lambda u, s: (0, 0), 0x4eba00: lambda u, s: (0, 0)})
    p.freeze_hooks()
    uc.mem_write(U, bytes(0x200)); uc.mem_write(D, bytes(0x300))
    put(U + 0xb4, D); put16(D + 0x232, LEASH)            # maneuverleashlength
    put(U + 0x68, UX << 16); put(U + 0x70, UZ << 16)

    rows, expected = [], []
    for mover in (1, 0):
        put(U + 8, MOVER if mover else 0)
        for point in (1, 0):
            for stance in range(3):
                for fire in range(3):
                    for explicit, force in ((1, 0), (0, 0), (0, 1)):
                        put(U + 0x130, 0x1000000 | STANCE[stance] | FIRE[fire])
                        log.clear()
                        r, err = p.call(0x4d7de0, (U, T, explicit, U + 0x68 if point else 0, 1 - explicit, force))
                        assert err is None, err
                        parts = [str(r)]
                        for m in log:
                            if m['kind'] == ATTACK:
                                leashed = m['leash'] != 0   # the leashed branch alone writes the origin
                                origin = (get16(m['this'] + 0x2e), get16(m['this'] + 0x30)) if leashed else (0, 0)
                                parts.append(f"attack:{m['mode']}:{m['leash'] if leashed else 0}:{origin[0]}:{origin[1]}")
                            else:
                                parts.append(f"return:{m['mode']}:{m['goal'][0]}:{m['goal'][1]}")
                        expected.append(' '.join(parts))
                        rows.append(f'{mover} {point} {stance} {fire} {explicit} {force} {UX} {UZ} {LEASH}')
    proc = subprocess.run([args.binary, '--gate'], input='\n'.join(rows) + '\n', text=True,
                          capture_output=True, check=True)
    actual = proc.stdout.splitlines()
    if len(actual) != len(expected):
        raise AssertionError(f'{len(actual)} answers for {len(expected)} cases')
    bad = [(rows[i], want, got) for i, (want, got) in enumerate(zip(expected, actual)) if want != got]
    for row, want, got in bad:
        print(f'FAIL mover point stance fire explicit force = {row}\n  retail {want}\n  port   {got}')
    issued = sum(1 for e in expected if e[0] == '1')
    print(f'{"PASS" if not bad else "FAIL"}: {len(rows) - len(bad)}/{len(rows)} gate calls ({issued} issue missions) '
          'match retail')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
