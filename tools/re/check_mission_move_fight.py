#!/usr/bin/env python3
"""Check what the campaign `ma X Y` clause pushes, against retail.

Runs retail's mission-order parser 0x513fb0(unit, string, table) on `m X Y`
and `ma X Y` (both cases of the letters), with the order resolver 0x4de530
and the push 0x4d78a0 hooked to record every argument (the CRT's sscanf
stood in for, as check_mission_attack_point.py does). Then runs the mission
ctor 0x4d6c40 natively with the push's arguments to see where the extra one
lands.

It asserts the rule MissionScript::applyOrders ports (src/sim/mission.cpp):

- `m` and `ma` both ask the resolver for kind 2 (the plain move click) at
  the same point, and push it queued (flag 1) with no target and Ctrl 0;
- the only difference is push argument 6: 0 for `m`, -1 for `ma`;
- 4d78a0 hands it to 4d6c40, which stores it at mission +0x52, the move's
  response mode (Move_Ground / VTOL_Move scan on every stance when it is
  below 0, never at 0). That is the mission retail's own fight-move key
  command builds (5214b0 sets the same argument to -1 for a resolved
  Move_Ground, VTOL_Move or QMove, 521bf4..521c5b), so `ma` is World's
  attackMove leg (markFightLeg: response mode -1) and `m` a plain move
  (mode 0).

    python3 tools/re/check_mission_move_fight.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

GS, SCEN, UNIT, TEXT, MISSION = HEAP, HEAP + 0x20000, HEAP + 0x30000, HEAP + 0x40000, HEAP + 0x50000
GIVEN = 0x2a


def retail(order):
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    def geti(a): return struct.unpack('<i', uc.mem_read(a, 4))[0]
    put(0x62d55c, GS); put(GS + 0x175dc, SCEN)
    uc.mem_write(SCEN + 0xdc5, struct.pack('<I', 16))
    put(UNIT + 0x130, 0x1000020)
    uc.mem_write(TEXT, order.encode() + b'\0')
    trace = []
    def resolve(uc, a):
        out, kind, unit, target, point, extra = (get(a + 4 * i) for i in range(6))
        trace.append(('resolve', kind, unit == UNIT, target, tuple(geti(point + 4 * i) for i in range(3)), extra))
        uc.mem_write(out, bytes((GIVEN,)))
        return 6, out
    def push(uc, a):
        v = [geti(a + 4 * i) for i in range(11)]
        point = tuple(geti(v[4] + 4 * i) for i in range(3)) if v[4] else None
        trace.append(('push', v[0] & 0xff, v[1], v[2] == UNIT, v[3], point, v[5], v[6], v[7:]))
        return 11, 0
    def scan(uc, a):
        text = bytes(uc.mem_read(get(a), 256)).split(b'\0')[0].decode()
        fmt = bytes(uc.mem_read(get(a + 4), 16)).split(b'\0')[0]
        assert fmt == b' %f %f', fmt
        count = 0
        for word, out in zip(text.split()[:2], [get(a + 8), get(a + 12)]):
            try: value = float(word)
            except ValueError: break
            uc.mem_write(out, struct.pack('<f', value)); count += 1
        return 0, count
    icd.hooks.update({0x5d46f0: scan, 0x4de530: resolve, 0x4d78a0: push,
                      0x4d4bf0: lambda uc, a: (1, 0)})
    _, error = icd.call(0x513fb0, (UNIT, TEXT, 0))
    assert not error, error
    return trace


def stored(extra):
    """4d6c40(mission, target 0, &point, 0, extra, 0, ...) -> the mission's +0x4e, +0x52, +0x56."""
    icd = Icd(); uc = icd.uc
    uc.mem_write(MISSION, bytes(0x80))
    uc.mem_write(GS, bytes(0x20000)); uc.mem_write(0x62d55c, struct.pack('<I', GS))
    point = MISSION + 0x100
    uc.mem_write(point, struct.pack('<iii', 80 << 16, 0, 96 << 16))
    icd.hooks[0x519990] = lambda uc, a: (2, 0)
    icd.hooks[0x4d4cc0] = lambda uc, a: (0, MISSION + 0x200)   # the descriptor row (flags read after +0x52)
    args = (GIVEN, 0, point, 0, extra, 0) + (0,) * 6
    _, error = icd.call(0x4d6c40, args, ecx=MISSION)
    assert not error, error
    return struct.unpack('<iii', uc.mem_read(MISSION + 0x4e, 12))


def main():
    p = (5 * 65536 * 16, 0, 7 * 65536 * 16)
    plain = ('resolve', 2, True, 0, p, 0)
    for text, extra in (('m 5 7', 0), ('M 5 7', 0), ('ma 5 7', -1), ('mA 5 7', -1), ('MA 5 7', -1)):
        got = retail(text)
        assert got[0] == plain, (text, got)
        assert got[1] == ('push', GIVEN, 1, True, 0, p, 0, extra, [0, 0, 0, 0]), (text, got[1])
    # A letter after `m` that is not a: the scan starts after `m`, so `mx 5 7` reads nothing.
    assert retail('mx 5 7') == [], 'mx'
    for extra in (0, -1):
        assert stored(extra) == (0, extra, 0), (extra, stored(extra))
    print('PASS: m and ma both resolve kind 2 at the same point and push it queued; '
          'ma passes -1 where m passes 0, and 4d6c40 stores it as the move\'s response mode (+0x52)')


if __name__ == '__main__':
    main()
