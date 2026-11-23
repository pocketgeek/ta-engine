#!/usr/bin/env python3
"""Retail shared sight between allies, emulated (2026-11-03).

Runs the real 0x4f6770 (game start: default ShareMapAndLOS between allied
players), the real alliance iterator 0x50e490, the real sight-object setup
0x4c66b0 (which player's plane a unit stamps into) and the real LosOn test
0x4f7210 (KINGDOMS.icd).  Unhooked.  Player records are game+0x2404 +
0x110 * i: +0 present, +0x50 the lobby slot (+0x84 type, 2 = computer;
+0x8a team, 4 = none), +0x88 sight plane, +0xea kind (1 local human,
2 local computer, 3 remote), +0xeb index, +0xf7[j] "j's sight comes to me",
+0x101[j] "my sight goes to j".  Observation only; reads the user's own
retail binary; nothing is copied."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

p = Icd(); uc = p.uc
put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
put8 = lambda a, v: uc.mem_write(a, bytes((v & 0xff,)))
put16 = lambda a, v: uc.mem_write(a, struct.pack('<H', v & 0xffff))
get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
get8 = lambda a: uc.mem_read(a, 1)[0]

GAME = HEAP + 0x40000
LOBBY = HEAP + 0x60000
PLANES = HEAP + 0x70000
UNIT, DEF, SIGHT = HEAP + 0x90000, HEAP + 0x91000, HEAP + 0x92000
GLOB_P, SETTINGS = HEAP + 0x93000, HEAP + 0x93100
W = H = 32
uc.mem_write(GAME, bytes(0x1a000))
put(0x62d55c, GAME)
put(0x62d558, GLOB_P); put(GLOB_P + 8, SETTINGS); put8(SETTINGS + 0x15, 1)   # LosOn
rec = lambda i: GAME + 0x2404 + 0x110 * i

checks = 0
def check(ok, label):
    global checks
    print(('ok   ' if ok else 'FAIL ') + label)
    if not ok: raise SystemExit(1)
    checks += 1

def setup(players, local=0):
    """players: list of (kind, lobby type, team)."""
    uc.mem_write(GAME + 0x2404, bytes(0x110 * 10))
    for i in range(10):            # 0x50e490 reads every row's lobby slot
        uc.mem_write(LOBBY + 0x100 * i, bytes(0x100)); put(rec(i) + 0x50, LOBBY + 0x100 * i)
        put8(LOBBY + 0x100 * i + 0x8a, 4)
    for i, (kind, ltype, team) in enumerate(players):
        r, l = rec(i), LOBBY + 0x100 * i
        uc.mem_write(l, bytes(0x100))
        put(r, 1); put(r + 0x50, l); put8(l + 0x84, ltype); put8(l + 0x8a, team)
        put8(r + 0xea, kind); put8(r + 0xeb, i)
        put8(r + 0xf7 + i, 1); put8(r + 0x101 + i, 1)          # 0x4f6290: self
        put(r + 0x88, PLANES + 0x1000 * i); put(r + 0x8c, W); put(r + 0x90, H)
        uc.mem_write(PLANES + 0x1000 * i, bytes(W * H))
    put8(GAME + 0x306e, local)
    r, err = p.call(0x4f6770, ()); assert err is None, err
    n = len(players)
    inc = [[get8(rec(i) + 0xf7 + j) for j in range(n)] for i in range(n)]
    out = [[get8(rec(i) + 0x101 + j) for j in range(n)] for i in range(n)]
    return inc, out

HUMAN, AI_LOCAL, REMOTE = 1, 2, 3
PLAYER, COMPUTER = 1, 2
# 0 local human team 0, 1 remote human team 0, 2 local computer team 0,
# 3 remote human team 1, 4 remote human no team (4), 5 remote human no team.
players = [(HUMAN, PLAYER, 0), (REMOTE, PLAYER, 0), (AI_LOCAL, COMPUTER, 0),
           (REMOTE, PLAYER, 1), (REMOTE, PLAYER, 4), (REMOTE, PLAYER, 4)]
inc, out = setup(players)
for i, row in enumerate(inc): print(f'   player {i} +0xf7 {row}  +0x101 {out[i]}')
check(inc[0][1] == inc[1][0] == 1 and out[0][1] == out[1][0] == 1,
      '4f6770: two humans on one team share both ways (+0xf7 and +0x101 set)')
check(inc[0][2] == inc[2][0] == 0 and out[0][2] == out[2][0] == 0 and inc[1][2] == inc[2][1] == 0,
      '4f6770: a computer on the same team shares with nobody, and nobody with it')
check(inc[0][3] == inc[3][0] == 0, '4f6770: humans on different teams do not share')
check(inc[4][5] == inc[5][4] == 0, '4f6770: two players with no team (4) do not share')
check(all(inc[i][i] for i in range(6)), 'own index stays set (0x4f6290)')
# Two computers on one team.
inc, out = setup([(HUMAN, PLAYER, 1), (AI_LOCAL, COMPUTER, 0), (AI_LOCAL, COMPUTER, 0)])
check(inc[1][2] == inc[2][1] == 0, '4f6770: two allied computers do not share')

# Which plane a unit's sight stamps into (0x4c66b0): the LOCAL player's
# record when local+0xf7[unit owner] is set, else the owner's own record.
def sight_owner(owner, local):
    uc.mem_write(UNIT, bytes(0x138)); uc.mem_write(DEF, bytes(0x300)); uc.mem_write(SIGHT, bytes(0x40))
    put(UNIT + 0xb4, DEF); put(UNIT + 0xb8, rec(owner)); put8(UNIT + 0xfd, owner); put16(DEF + 0x226, 200)
    put8(GAME + 0x306e, local)
    r, err = p.call(0x4c66b0, (UNIT,), ecx=SIGHT); assert err is None, err
    return get(SIGHT), struct.unpack('<h', uc.mem_read(SIGHT + 0x1c, 2))[0]
setup(players)
plane, dist = sight_owner(1, 0)
check(plane == rec(0), f'4c66b0 on player 0\'s machine: ally 1\'s unit stamps into player 0\'s plane ({plane:#x})')
check(dist == 200, f'4c66b0: the sight object\'s distance is def+0x226 as authored ({dist})')
plane, _ = sight_owner(2, 0)
check(plane == rec(2), f'4c66b0: the allied computer\'s unit stamps into its own plane ({plane:#x})')
plane, _ = sight_owner(3, 0)
check(plane == rec(3), ' 4c66b0: an enemy\'s unit stamps into its own plane')
plane, _ = sight_owner(0, 1)
check(plane == rec(1), '4c66b0 on player 1\'s machine: player 0\'s unit stamps into player 1\'s plane')

# 4f7210 reads only the viewer's plane: an enemy on a cell lit only in the
# ally's plane is not seen; after sharing, the ally's units light the
# viewer's own plane (4c66b0 above), which 4f7210 reads.
E = HEAP + 0x94000; ED = HEAP + 0x95000
uc.mem_write(E, bytes(0x138)); uc.mem_write(ED, bytes(0x300))
put(E + 0xb4, ED); put(E + 0xb8, rec(3)); put8(E + 0xfd, 3); put(E + 0x130, 0x1000000 | 1)
put(E + 0x68, 400 << 16); put(E + 0x70, 400 << 16)
cell = (400 >> 5) * W + (400 >> 5)
put8(PLANES + 0x1000 * 1 + cell, 1)
r, _ = p.call(0x4f7210, (rec(0), E)); check(r == 0, '4f7210: enemy lit only in ally 1\'s plane: not seen by player 0')
put8(PLANES + 0x1000 * 0 + cell, 1)
r, _ = p.call(0x4f7210, (rec(0), E)); check(r == 1, '4f7210: the same cell lit in player 0\'s own plane: seen')
print(f'PASS: {checks} observations')
