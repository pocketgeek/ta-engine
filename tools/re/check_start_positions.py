#!/usr/bin/env python3
"""Check retail's fixed start locations: the k-th occupied battle-room row takes
the k-th start (docs/retail-engine.md, "Start positions").

Builds synthetic player records (gs+0x2404, stride 0x110; one per battle-room
row) for every seating of the eight rows and runs, with fixed starts:

  * the skirmish spawn 0x4d2e70 (map mode 2), hooking 0x4d3200(player, start)
    to log which start each player is given;
  * the multiplayer host's start table 0x4ea950 (map mode 3), stubbing the
    network sends and reading the per-row table it leaves at gs+0x3030.

and compares both against the rule setupMatch follows and start_positions_test
pins. Observation only: it reads YOUR OWN retail install from the gitignored
assets/ directory, and nothing from the binary is copied into the engine.

    pip install unicorn && python3 tools/re/check_start_positions.py
"""
import itertools
import struct
import sys

from emu import Icd, HEAP

GS = HEAP + 0x100000          # game state; the routines reach past +0x19e90
MAPOBJ = HEAP + 0x1000        # [gs+0x175dc]: dword 0 is the map mode
SETROOT, SETTINGS = HEAP + 0x2000, HEAP + 0x2100   # [[0x62d558]+8]+0x14: random starts
THIS = HEAP + 0x3000
PINFO = HEAP + 0x4000         # record+0x50 -> per-player info, 0x100 apart
REC0, RECSZ, NREC = 0x2404, 0x110, 10
HUMAN, AI, REMOTE = 1, 2, 3   # record kind (+0xea)


def setup(icd, mode, kinds):
    uc = icd.uc
    w32 = lambda a, v: uc.mem_write(a, struct.pack('<I', v))
    w8 = lambda a, v: uc.mem_write(a, bytes([v]))
    uc.mem_write(GS, bytes(0x20000))
    w32(0x62d55c, GS)
    w32(GS + 0x175dc, MAPOBJ); w32(MAPOBJ, mode)
    w32(0x62d558, SETROOT); w32(SETROOT + 8, SETTINGS)
    uc.mem_write(SETTINGS, bytes(0x40))               # +0x14 = 0: fixed starts
    uc.mem_write(THIS, bytes(0x40))                   # +4 = 0: not a loaded game
    w8(GS + 0x306e, 0)                                # the local player is row 1
    for i in range(NREC):
        rec, info = GS + REC0 + i * RECSZ, PINFO + i * 0x100
        uc.mem_write(info, bytes(0x100))              # +0x9a/+0x9b flags clear
        w32(rec + 0x50, info)
        w8(rec + 0xeb, 10)                            # no player number
        if i in kinds:                                # as 0x4f6290 seats a row
            w32(rec, 1)
            w8(rec + 0xea, kinds[i])
            w8(rec + 0xeb, i)
            w8(rec + 0xec, i)
            w8(rec + 0x4e, 1)                         # remote "joined" bit 0x4ea950 waits on


def skirmish(kinds):
    icd = Icd()
    got = {}

    def spawn(uc, argp):                              # 0x4d3200(player, start), stdcall
        player, start = struct.unpack('<ii', uc.mem_read(argp, 8))
        got[player] = start + 1                       # the .ota parse stores StartPosN as N-1
        return 2, 0
    icd.hooks[0x4d3200] = spawn
    setup(icd, 2, kinds)
    _, err = icd.call(0x4d2e70, ecx=THIS)
    if err:
        raise RuntimeError(err)
    return got


def mphost(kinds):
    icd = Icd()
    icd.hooks[0x4eb350] = lambda uc, a: (0, 1)        # every player has joined
    for fn, nargs in ((0x4e6860, 1), (0x4e6780, 3), (0x4e7e00, 2), (0x4e8030, 3)):
        icd.hooks[fn] = (lambda n: lambda uc, a: (n, 0))(nargs)   # network sends: no-op
    setup(icd, 3, kinds)
    icd.call(0x4ea950, allow_early_stop=True)         # stops after the sends we stubbed
    table = icd.uc.mem_read(GS + 0x3030, 4 * NREC)
    return {i: table[4 * i] + 1 for i in range(NREC) if table[4 * i] != 0xff}


def rule(kinds):                                      # the k-th seated row takes start k
    return {row: k + 1 for k, row in enumerate(sorted(kinds))}


def main():
    bad = 0
    seatings = [s for n in range(1, 9) for s in itertools.combinations(range(8), n)]
    for seats in seatings:
        kinds = {r: (HUMAN if r == seats[0] else AI) for r in seats}
        if skirmish(kinds) != rule(kinds):
            bad += 1
            print('skirmish', seats, skirmish(kinds), 'expected', rule(kinds))
    for seats in (s for s in seatings if s[0] == 0):  # the host sits in row 1
        kinds = {r: (HUMAN if r == 0 else REMOTE if r % 2 else AI) for r in seats}
        if mphost(kinds) != rule(kinds):
            bad += 1
            print('multiplayer', seats, mphost(kinds), 'expected', rule(kinds))
    example = {0: HUMAN, 7: AI}
    print('human row 1 + AI row 8: skirmish %s, multiplayer host %s' % (
        ' '.join('row %d->StartPos%d' % (r + 1, n) for r, n in sorted(skirmish(example).items())),
        ' '.join('row %d->StartPos%d' % (r + 1, n) for r, n in sorted(mphost(example).items()))))
    print('%d skirmish + %d multiplayer seatings, %d disagree with "k-th seated row takes start k"'
          % (len(seatings), sum(1 for s in seatings if s[0] == 0), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
