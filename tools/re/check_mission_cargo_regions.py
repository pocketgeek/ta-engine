#!/usr/bin/env python3
"""Check which trigger regions a unit aboard a transport is in, against retail.

Retail runs natively, in one emulator, for each case:
  - 4dad30, the mover commit, on a passenger whose carrier pointer (+0xa8)
    is set (attach piece -1, as both pickup handlers pass): its attached
    branch takes the carrier's attach point (4dd250) and stores it with
    51b3b0, which also stores the footprint-origin cell +0x74/+0x76 from
    the passenger's OWN footprint (+0x78) when the cell changes (the
    footprint stamps 506aa0/5066f0 hooked out). Stopped at 4dadd7, as
    check_cargo_attach_position.py stops it;
  - then 4d33a0, the per-unit region check, on that passenger with the
    carried state left set (+0xa8 the carrier, every +0x114 bit set): the
    TriggerHit calls it makes (the script thread start 56c640 hooked).

The rule checked: a passenger counts for the regions, at the cell
footprintOrigin(carrier position, passenger footprint) -- not skipped, and
not at the carrier's own cell. World answers it through
`mission_orders_test --regions` (origin, rect, circle), whose sweep now
reads an embarked unit's position, which World's cargo step keeps at the
carrier's attach point (check_cargo_attach_position.py).

    python3 tools/re/check_mission_cargo_regions.py --binary build-o2/mission_orders_test
"""
import argparse
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP
import check_mission_regions as regions

RECTS = {0: (10, 20, 12, 22), 1: (30, 30, 30, 30), 4: (100, 7, 140, 9)}
CIRCLES = {5: (30, 30, 4), 7: (60, 60, 7)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/mission_orders_test')
    ap.add_argument('--cases', type=int, default=600)
    args = ap.parse_args()
    t = regions.Triggers()
    icd, put, get, cstr, st = t.e
    uc = icd.uc
    for a in (0x506aa0, 0x5066f0):
        icd.hooks[a] = lambda uc, ar: (1, 0)
    icd.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x4dadd7, end=0x4dadd7)
    for rid, r in RECTS.items(): assert regions.command(t.e, 'SetTrigger', (rid,) + r) == rid
    for rid, c in CIRCLES.items(): assert regions.command(t.e, 'SetTrigger', (rid,) + c) == rid
    host = regions.UNITS + 30 * regions.UNIT_SIZE
    kind, mover = regions.HEAP + 0x300000, regions.HEAP + 0x301000
    uc.mem_write(kind, bytes(0x400)); uc.mem_write(mover, bytes(0x100))
    put(mover + 0x36, '<H', 1)
    rng = random.Random(0x4d33a0)
    # Carrier positions around and inside every region, passengers of
    # footprints 1..4 (a 1x1 on a 3x3 ship lands a cell off its carrier).
    spots = [(cx * 16 + d, cz * 16 + e) for cx, cz in ((11, 21), (30, 30), (60, 60), (120, 8), (34, 30), (67, 60))
             for d in (0, 7, 8, 15) for e in (0, 8)]
    spots += [(rng.randrange(0, 2400), rng.randrange(0, 1200)) for _ in range(args.cases)]
    queries, expect, hits_seen = [], [], 0
    for i, (hx, hz) in enumerate(spots):
        fx, fz = rng.randrange(1, 5), rng.randrange(1, 5)
        cargo = t.unit(i % 25, 0x7fff, 0x7fff)        # last stored cell far from any region
        put(cargo + 0x78, '<hh', fx, fz); put(cargo + 0xa8, '<I', host); put(cargo + 0xb4, '<I', kind)
        put(cargo + 0x112, '<b', -1); put(cargo + 0x114, '<B', 0xff)
        put(host + 0x68, '<iii', hx << 16, 0, hz << 16); put(host + 0xc0, '<I', 0)
        _, err = icd.call(0x4dad30, (cargo,), ecx=mover, allow_early_stop=True)
        assert not err and uc.reg_read(UC_X86_REG_EIP) == 0x4dadd7, err
        assert struct.unpack('<ii', uc.mem_read(cargo + 0x68, 4) + uc.mem_read(cargo + 0x70, 4)) == (hx << 16, hz << 16)
        ox, oz = struct.unpack('<hh', uc.mem_read(cargo + 0x74, 4))
        got = {c[1] for c in t.check(cargo) if c[0] == 'TriggerHit'}
        hits_seen += len(got)
        queries += [f'origin {hx << 16} {fx}', f'origin {hz << 16} {fz}']
        expect += [str(ox), str(oz)]
        for rid, (x1, z1, x2, z2) in RECTS.items():
            queries.append(f'rect {x1} {z1} {x2} {z2} {ox} {oz}'); expect.append('1' if rid in got else '0')
        for rid, (cx, cz, r) in CIRCLES.items():
            queries.append(f'circle {cx} {cz} {r} {ox} {oz}'); expect.append('1' if rid in got else '0')
    assert hits_seen > 0, 'no case put a passenger in a region'
    out = subprocess.run([args.binary, '--regions'], input='\n'.join(queries) + '\n', text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(out) == len(queries), (len(out), len(queries))
    bad = [(q, e, p) for q, e, p in zip(queries, expect, out) if e != p]
    if bad:
        raise AssertionError({'mismatches': len(bad), 'first': bad[:8]})
    print(f'PASS: {len(spots)} carried passengers: 4dad30 moves the origin cell with the carrier '
          f'(own footprint), 4d33a0 fires {hits_seen} TriggerHits for them; {len(queries)} answers match')


if __name__ == '__main__':
    main()
