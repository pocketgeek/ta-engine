#!/usr/bin/env python3
"""Check where an unload puts the released passenger's PARK, against retail.

Runs stage 4 of Ground_Unload (408d50) and VTOL_Unload (41ae20) natively, the
callees sunk as tools/re/probe_transport_unload.py sinks them, with the
CARRIER's +0x130 bits, construction left (+0x108), +0x104 and its own carrier
(+0xa8, with or without the holder bit) varied. Before the detach each
handler computes the selection predicate on the carrier (40901c..40906e,
41b394..41b3e9): live, not dying, selectable (0x20), finished, +0x104 == 0,
not carried or carried by a holder (0x4000000). The PARK push (4d78a0) then
gets (S, E here):

  * +0x52, the park's "permanent" (4d6c40 stores push argument 6 there): 0
    always;
  * push argument 7, which 4d78a0 reads at 4d7a09: 0 appends the PARK to the
    passenger's queue (4d77f0), not 0 puts it in front (4d7750). It is the
    predicate's negation: a carrier the owner could select appends, any other
    (a mission script's transport, one carried, unfinished, dying) puts the
    PARK first.

and requires `transport_test --unload-park-order` (World::unloadParkAhead,
the port) to give the same answers.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_unload_park_order.py [build/transport_test]
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def run(handler, flags, left, held, carried, holder):
    p = Icd()
    carrier, passenger, kind, mission, game, outer = [HEAP + i * 0x10000 for i in range(6)]
    def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def read(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    pushes = []
    def detach(uc, sp):
        put(passenger + 0xa8, 0); put(carrier + 0xac, 0); return 5, 0
    def park(uc, sp):
        pushes.append(list(struct.unpack('<11i', uc.mem_read(sp, 44)))); return 11, 0
    def bind(uc, sp):
        ref = uc.reg_read(UC_X86_REG_ECX); put(ref + 4, read(sp)); return 1, ref
    controller = HEAP + 0x70000
    p.hooks.update({0x4eb9e0: lambda uc, sp: (0, controller), 0x4e40e0: lambda uc, sp: (2, controller),
                    0x4e4540: lambda uc, sp: (1, 0), 0x4d4d40: lambda uc, sp: (1, 0), 0x5199f0: bind,
                    0x4d4da0: lambda uc, sp: (2, 0), 0x51b480: lambda uc, sp: (4, 0), 0x51b4f0: detach,
                    0x535cc0: lambda uc, sp: (1, 0), 0x4d4bf0: lambda uc, sp: (1, 0), 0x4d78a0: park,
                    0x519ef0: lambda uc, sp: (0, 0), 0x507d10: lambda uc, sp: (5, 1),
                    0x4d6a10: lambda uc, sp: (1, 0), 0x50a9c0: lambda uc, sp: (3, 0),
                    0x421e10: lambda uc, sp: (2, 0), 0x4f5db0: lambda uc, sp: (2, 0)})
    p.freeze_hooks()
    put(0x62d55c, game); put(game + 0x19f30, 1); put(game + 0x174c8, 101); put(game + 0x174cc, 102)
    put(carrier + 0xb4, kind); put(kind + 0x264, 0x200); put(carrier + 0xac, passenger)
    put(carrier + 0x68, 80 << 16); put(carrier + 0x6c, 100 << 16); put(carrier + 0x70, 240 << 16)
    put(carrier + 0x130, flags); p.uc.mem_write(carrier + 0x108, struct.pack('<f', left))
    put(carrier + 0x104, held)
    if carried:
        put(carrier + 0xa8, outer); put(outer + 0x130, 0x1000000 | (0x4000000 if holder else 0))
    put(passenger + 0x130, 0x1000000); put(passenger + 0xa8, carrier); put(passenger + 0xb4, kind)
    put(mission + 0x16, passenger)
    put(mission + 0x22, (160 << 16) + 123); put(mission + 0x26, 20 << 16); put(mission + 0x2a, (320 << 16) + 456)
    p.uc.mem_write(mission + 5, bytes((4,)))
    result, error = p.call(handler, (carrier, mission, 0))
    assert not error, error
    assert len(pushes) == 1, pushes
    v = pushes[0]
    return v[6], int(v[7] != 0)        # +0x52 (permanent), put ahead


CASES = [  # flags, construction left, +0x104, carried, holder
    (0x1000020, 0.0, 0, False, False),
    (0x1000000, 0.0, 0, False, False),
    (0x1001020, 0.0, 0, False, False),
    (0x1000020, 0.5, 0, False, False),
    (0x1000020, 0.0, 150, False, False),
    (0x1000020, 0.0, 0, True, False),
    (0x1000020, 0.0, 0, True, True),
    (0x1000000, 0.0, 0, True, True),
]


def main():
    rows = []
    for handler in (0x408d50, 0x41ae20):
        for flags, left, held, carried, holder in CASES:
            permanent, ahead = run(handler, flags, left, held, carried, holder)
            assert permanent == 0, (hex(handler), permanent)
            rows.append(f'{flags:#x} {left} {held} {int(carried)} {int(holder)} {ahead}')
    # Both handlers agree; the port answers each case once. +0x104 and the
    # holder bit are never set in lockstep (+0x104 only for a remote owner,
    # 514e26; the holder is an isairbase carrier, which no shipped unit is),
    # so the port is asked only the cases without them.
    half = len(rows) // 2
    assert rows[:half] == rows[half:], rows
    asked = [r for r in rows[:half] if r.split()[2] == '0' and r.split()[4] == '0']
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'transport_test')
    queries = '\n'.join(' '.join(r.split()[:5]) for r in asked) + '\n'
    ours = subprocess.run([binary, '--unload-park-order'], input=queries, capture_output=True, text=True,
                          check=True).stdout.split()
    want = [r.split()[5] for r in asked]
    if ours != want:
        raise SystemExit(f'FAIL: retail {want}, ours {ours}')
    print(f'PASS: {len(rows)} unload stage-4 releases (air and sea): the PARK is never permanent (+0x52 0), and '
          'it goes in front of the passenger\'s orders unless the carrier is live, selectable, finished, free '
          'and not carried (or carried by a holder); the port matches')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--retail':
        for h in (0x408d50, 0x41ae20):
            for c in CASES: print(hex(h), c, run(h, *c))
    else:
        main()
