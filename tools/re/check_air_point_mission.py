#!/usr/bin/env python3
"""Check the mission a flyer's attack at a point gets, as the Retail AI reads it.

The order resolver's flyer branch (4de770..4de7e3) names AIRSTRIKE,
AIRTOGROUNDHOVER or AIRTOGROUND (0x615dac / 0x615d98 / 0x615d8c). This runs
retail's descriptor lookup 0x4d4bf0 on each name over a sorted definition
table made of the shipped rows (the runtime table at 0x62db84 is sorted by
name; only the rows this check needs are put in it), then retail's mission
ctor 0x4d6c40 with no target unit and a point, and reads the mission's class
(definition +0x10) and its flags (+0x5a), the two the AI reads.

It asserts what ClassicBrain's firstMission ports (src/ai/classicbrain.cpp):

- AIRSTRIKE at a point: class 2, flags 0x1000400 (row 0x1000600; the ctor
  clears the target bit 0x200 without a target, 4d6d66);
- AIRTOGROUNDHOVER and AIRTOGROUND at a point: class 1, flags 0x1000000
  (row 0x1000200, the target bit cleared);
- the controls: SUPPRESS at a point keeps 0x1000410 (no 0x200 to clear),
  VTOL_SEEKATTACK at a point reads 0x1000400 (as firstMission's seekAttack),
  AIRTOGROUND with a live target keeps 0x1000200, and with no point the ctor
  also clears 0x400 (4d6d6f).

A flyer's attack on a unit (the resolver's unit branch, 4de8b0..4de9ea) and
its patrol, as ClassicBrain's firstMission reads them:

- AIRTOAIR, AIRTOGROUND and AIRTOGROUNDHOVER with a live target: class 1,
  0x1000200 (not Attack_Chase's 0x1000280);
- AIRSTRIKE with a live target: class 2, 0x1000600 with the point a player's
  order passes (521a66..521abc, the target's +0x68), 0x1000200 without one
  (an automatic attack, 4d7de0 passes none: 4d8027..4d8035);
- VTOL_PATROL at a point: class 7, 0x1000412 (not Patrol's 0x3000410).

A flyer's move, guard and the group missions (fx-groups, 2026-11-05):

- VTOL_MOVE at a point: class 14, 0x1000402 (not Move_Ground's 0x3000400:
  no navigator formation bit 0x2000000);
- VTOL_FOLLOW on a live ward: class 5, 0x1000200 (not Follow_Ground's
  0x3000200); VTOL_EVADE on a live target 0x1000200;
- VTOL_MOVE_FORMATION and VTOL_MOVE_FORMATIONSTANDBY as the group checks
  build them (4d7750 with no target and no point): 0x1000002 and 0x5000002;
- VTOL_STANDBY (the FBI default, no point): 0x5010000.

Then the patrol's stage 0 (41a64f, 4d6b80): with no mission in the list
flagged 0x4000 it appends a VTOL_PATROL at the unit's position (+0x68, no
target), 0x1000412, and flags the outbound mission 0x4000: 0x1004412; with
one already flagged it appends nothing and still flags it. World's flyer
patrol legs carry these flags (sim.cpp patrol, patrolTo; retailMissionFlags
reads them for the group's moving aggregate).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_air_point_mission.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

# The shipped definition rows (25 bytes: +0 code name, +4 handler, +0x10
# class, +0x11 flags, +0x15 name), by the row's address in .rdata.
ROWS = {
    'AirStrike': 0x5ebd69, 'AirToGround': 0x5ebd9b, 'AirToGroundHover': 0x5ebdb4,
    'Suppress': 0x5eb8b8, 'VTOL_SeekAttack': 0x5ebe7c, 'AirToAir': 0x5ebd82,
    'VTOL_Patrol': 0x5ebd50, 'Patrol': 0x5eb94e,
    'VTOL_Move': 0x5ebcd3, 'Move_Ground': 0x5eb86d, 'VTOL_Follow': 0x5ebd37, 'Follow_Ground': 0x5eb89f,
    'VTOL_Move_Formation': 0x5ebca1, 'VTOL_Move_FormationStandby': 0x5ebcba, 'VTOL_Standby': 0x5ebc88,
    'VTOL_Evade': 0x5ebe63,
}
TABLE, SLOT, MISSION, POINT, TARGET = HEAP, HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000


def setup():
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    def cstr(a): return bytes(uc.mem_read(a, 64)).split(b'\0')[0].decode()
    rows = []
    for row in ROWS.values():
        raw = bytes(uc.mem_read(row, 25))
        rows.append((cstr(struct.unpack_from('<I', raw, 0x15)[0]).lower(), raw))
    rows.sort()
    for i, (name, raw) in enumerate(rows):
        uc.mem_write(TABLE + 25 * i, raw)
    put(0x62db84, TABLE); put(0x62db88, TABLE + 25 * len(rows))
    put(0x62d55c, HEAP + 0x10000)
    uc.mem_write(POINT, struct.pack('<iii', 1760 << 16, 0, 320 << 16))
    return icd, put, get, cstr


def mission(name, target=False, point=True):
    icd, put, get, cstr = setup(); uc = icd.uc
    text = HEAP + 0x5000
    uc.mem_write(text, name.encode() + b'\0')
    _, error = icd.call(0x4d4bf0, (text,), ecx=SLOT)
    if error: raise AssertionError((name, error))
    kind = uc.mem_read(SLOT, 1)[0]
    row = TABLE + 25 * kind
    found = cstr(get(row + 0x15))
    if found.lower() != name.lower(): raise AssertionError((name, 'looked up', found))
    if target: put(TARGET + 0x130, 0x1000000)   # a live unit (4d6d54: 0x1000000 set, 0x1000 clear)
    args = (kind, TARGET if target else 0, POINT if point else 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    _, error = icd.call(0x4d6c40, args, ecx=MISSION)
    if error: raise AssertionError((name, error))
    return uc.mem_read(row + 0x10, 1)[0], get(MISSION + 0x5a)


def patrol_anchor(anchored):
    """4d6b80(unit, mission) on an outbound VTOL_PATROL: returns the outbound
    mission's flags and the flags of every mission it appended (4d77b0)."""
    icd, put, get, cstr = setup(); uc = icd.uc
    text = HEAP + 0x5000
    uc.mem_write(text, b'VTOL_PATROL\0')
    _, error = icd.call(0x4d4bf0, (text,), ecx=SLOT)
    if error: raise AssertionError(error)
    kind = uc.mem_read(SLOT, 1)[0]
    _, error = icd.call(0x4d6c40, (kind, 0, POINT, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=MISSION)
    if error: raise AssertionError(error)
    unit, other, arena = HEAP + 0x6000, HEAP + 0x7000, [HEAP + 0x8000]
    uc.mem_write(unit + 0x68, struct.pack('<iii', 160 << 16, 0, 200 << 16))
    # The unit's list (+0x60, next +0x66): the outbound mission, and in the
    # anchored case another mission already flagged 0x4000 behind it.
    put(unit + 0x60, MISSION); put(MISSION + 0x66, other if anchored else 0)
    put(other + 0x5a, 0x1004412 if anchored else 0); put(other + 0x66, 0)
    appended = []
    def allocate(uc, a):
        at = arena[0]; arena[0] += 0x100
        uc.mem_write(at, bytes(0x100))
        return 0, at   # cdecl: the caller pops the size
    def append(uc, a):
        u, m = get(a), get(a + 4)
        appended.append((u, get(m + 0x5a), struct.unpack('<iii', uc.mem_read(m + 0x22, 12))))
        return 2, 0
    icd.hooks[0x4eb9e0] = allocate
    icd.hooks[0x4d77b0] = append
    _, error = icd.call(0x4d6b80, (unit, MISSION))
    if error: raise AssertionError(error)
    for u, _, _ in appended:
        if u != unit: raise AssertionError(('appended to', hex(u)))
    return get(MISSION + 0x5a), [(hex(f), (p[0] >> 16, p[2] >> 16)) for _, f, p in appended]


def expect(name, cls, flags, **kw):
    got = mission(name, **kw)
    if got != (cls, flags):
        raise AssertionError({'mission': name, **kw, 'retail': (got[0], hex(got[1])), 'expected': (cls, hex(flags))})


def main():
    # The three a flyer's attack at a point gets (4de770..4de7e3): no target.
    expect('AIRSTRIKE', 2, 0x1000400)
    expect('AIRTOGROUNDHOVER', 1, 0x1000000)
    expect('AIRTOGROUND', 1, 0x1000000)
    # Controls.
    expect('SUPPRESS', 1, 0x1000410)
    expect('VTOL_SEEKATTACK', 19, 0x1000400)
    expect('AIRTOGROUND', 1, 0x1000200, target=True)
    expect('AIRSTRIKE', 2, 0x1000000, point=False)
    # A flyer's attack on a live unit (the resolver's unit branch).
    expect('AIRTOAIR', 1, 0x1000200, target=True)
    expect('AIRTOGROUNDHOVER', 1, 0x1000200, target=True)
    expect('AIRSTRIKE', 2, 0x1000600, target=True)
    expect('AIRSTRIKE', 2, 0x1000200, target=True, point=False)
    # A flyer's patrol, and the ground one it is not.
    expect('VTOL_PATROL', 7, 0x1000412)
    expect('PATROL', 7, 0x3000410)
    # A flyer's move, guard and the group missions.
    expect('VTOL_MOVE', 14, 0x1000402)
    expect('MOVE_GROUND', 14, 0x3000400)
    expect('VTOL_FOLLOW', 5, 0x1000200, target=True)
    expect('FOLLOW_GROUND', 5, 0x3000200, target=True)
    expect('VTOL_EVADE', 19, 0x1000200, target=True)
    expect('VTOL_MOVE_FORMATION', 14, 0x1000002, point=False)
    expect('VTOL_MOVE_FORMATIONSTANDBY', 14, 0x5000002, point=False)
    expect('VTOL_STANDBY', 15, 0x5010000, point=False)
    out, appended = patrol_anchor(False)
    if out != 0x1004412 or appended != [('0x1000412', (160, 200))]:
        raise AssertionError({'stage 0, no anchor': (hex(out), appended)})
    out, appended = patrol_anchor(True)
    if out != 0x1004412 or appended:
        raise AssertionError({'stage 0, anchored': (hex(out), appended)})
    print('PASS: 21 retail mission ctor cases: a flyer at a point reads AIRSTRIKE (2, 0x1000400), '
          'AIRTOGROUNDHOVER / AIRTOGROUND (1, 0x1000000); the target bit kept only with a target; '
          'a flyer on a unit reads class 1 0x1000200 (AIRSTRIKE 2, 0x1000600 with a point); '
          'VTOL_PATROL 0x1000412; VTOL_MOVE 0x1000402, VTOL_FOLLOW 0x1000200, the group formations '
          '0x1000002 / 0x5000002, VTOL_STANDBY 0x5010000; and 2 patrol stage-0 cases (4d6b80): the outbound leg 0x1004412, '
          'the anchor 0x1000412 at the unit only when none is flagged')


if __name__ == '__main__': main()
