#!/usr/bin/env python3
"""Compare World::changeOwner with retail's change of owner 514da0, natively.

514da0 runs under ShareUnits (4b6b33), the CRT transfer (4cb0c4), the mission
Capture (4d3d40) and the charm (52db08). This runs it whole, with 512060,
511f60 and 511990 under it native, on a synthetic game: a unit of player 0's
pool, player 1's pool of stale slots, a type table and the two owner
records. The routines that build a model, a script, a mover, weapon records
or a sight object, that stamp the ground, start a script or send a packet
are observation boundaries (recorded, not run); so are the game RNG 535cc0
(its bound is recorded and it answers 0) and the CRT rand (answers 0, the
first free slot). For every combination of: the new owner the same, the
unit dying (+0x130 & 0x1000), the new owner at its unit cap (game +0x14e7c
against record +0xe8), the type at its totalallowed (+0x136) for the new
owner, activatewhenbuilt (+0x260 & 0x40000), buildangle (+0x22e), a
movement speed (+0x162) and the old activation byte (+0x114, bit 1 active,
bit 4 cloaked), it requires from `owner_change_test --native`:

  * whether a new object is made, and the creation's draws in order: the
    CRT slot draw (5121d8), rand(buildangle) (511baa), rand(65536) for +0x82
    (511cae), rand(201) for the speed (512439) only for a mover;
  * the new object's activation byte (514fd2 / 514fe4 copy the old one);
  * that it starts with an empty mana pool (+0xd8), zero HP samples
    (+0x110/+0x111), self-repair carry (+0x10e), cloak delay (+0xcc) and
    air-traffic count (+0x12a), no last hitter (+0xf8 = 0, +0xfc = 10), no
    scenario name (+4, 51f4c0's), and keeps HP (+0x10c) raw.

Natively it also asserts what the port does not answer row by row: the
capture event 523570 comes first, before 512060 (so it is sent even when the
cap refuses the new object), nothing at all happens for the same owner or a
dying unit, the trigger latch +0xd4 is 0, the old object is killed with
51a140(0, old, 30000, 6, 0) only once the new one stands, the conditions'
creation event 5235d0 and the scenario's +0x14 run for the new object, and
the script calls are Activate / Deactivate on the edges of the copy. And the
FBI parser's activatewhenbuilt default (4c0327..4c035f) is 0.

    python3 tools/re/check_change_owner.py [build-o2/owner_change_test]
"""
import itertools
import struct
import subprocess
import sys

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ECX

UNIT = 0x138
TYPE = 0x2a4
ALIVE, DYING = 0x1000000, 0x1000


class Fixture:
    def __init__(self):
        self.p = Icd()
        (self.game, self.types, self.units, self.cond, self.scen, self.vt) = (HEAP + i * 0x40000 for i in range(6))
        self.log, self.draws = [], []
        self.remaining = 0.0   # the old object's +0x108 (unfinished())
        p = self.p

        def rec(name, n, ret=0, thiscall=False):
            def hook(uc, sp):
                args = struct.unpack('<%dI' % n, uc.mem_read(sp, 4 * n)) if n else ()
                self.log.append((name,) + tuple(args))
                return n, ret(args) if callable(ret) else ret
            return hook

        def game_rand(args):
            self.draws.append(args[0])
            return 0

        def crt_rand(uc, sp):
            self.draws.append(-1)
            return 0, 0

        def script(uc, sp):
            name = struct.unpack('<I', uc.mem_read(sp, 4))[0]
            self.log.append(('script', bytes(uc.mem_read(name, 16)).split(b'\0')[0].decode()))
            return 3, 0
        p.hooks[0x523570] = rec('captured', 1)
        p.hooks[0x5235d0] = rec('created', 1)
        p.hooks[0x51a140] = rec('damage', 5)
        p.hooks[0x5d4444] = crt_rand
        p.hooks[0x535cc0] = rec('rand', 1, ret=game_rand)
        p.hooks[0x56c5c0] = script
        p.hooks[self.vt + 0x100] = rec('scenario+0x14', 1)
        for address, n, name in ((0x511d00, 1, 'model'), (0x52aae0, 1, 'weapons'), (0x511f20, 1, 'mover'),
                                 (0x51a880, 2, 'point'), (0x519b10, 1, 'auto'), (0x50b4f0, 2, 'sound'),
                                 (0x4c66b0, 1, 'sight'), (0x51b2a0, 1, 'stamp'), (0x5066f0, 1, 'ground'),
                                 (0x4ea330, 1, 'place'), (0x4ea3b0, 2, 'place2'), (0x519a90, 2, 'cloaked'),
                                 (0x4e8030, 3, 'packet'), (0x4e68a0, 1, 'packet-id')):
            p.hooks[address] = rec(name, n)
        p.freeze_hooks()

    def put(self, address, value, fmt='I'):
        self.p.uc.mem_write(address, struct.pack('<' + fmt, value))

    def get(self, address, fmt='I'):
        return struct.unpack('<' + fmt, self.p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    def run(self, same, dying, capped, limited, flag, spread, moving, act):
        f = self
        for address in (f.game, f.types, f.units, f.cond, f.scen, f.vt):
            f.p.uc.mem_write(address, bytes(0x40000))
        f.log.clear(); f.draws.clear()
        f.put(0x62d55c, f.game); f.put(0x62a728, 0); f.put(0x62da3c, f.scen); f.put(f.scen, f.vt)
        f.put(f.vt + 0x14, f.vt + 0x100); f.put(f.game + 0x19f54, f.cond)
        kind = 1
        tp = f.types + kind * TYPE
        f.put(f.game + 0x175b8, 4); f.put(f.game + 0x175c4, f.types)
        f.put(tp + 0x260, 0x800000 | (0x40000 if flag else 0) | 0x15)
        f.put(tp + 0x136, 1 if limited else 0xffffffff)
        f.p.uc.mem_write(tp + 0x24a, bytes([1]))
        f.put(tp + 0x22e, spread, 'H'); f.put(tp + 0x126, 0x00020002); f.put(tp + 0x1be, 500, 'H')
        f.put(tp + 0x162, 0x20000 if moving else 0); f.put(tp + 0x1a2, 100.0, 'f')
        f.put(f.game + 0x14e7c, 100, 'H')
        records = []
        for player in range(2):
            r = f.game + 0x2404 + player * 0x110
            f.put(r, 1); f.p.uc.mem_write(r + 0xea, bytes([1, player]))
            f.put(r + 0x74, f.units + player * 4 * UNIT); f.put(r + 0x78, f.units + (player * 4 + 3) * UNIT)
            records.append(r)
        f.put(f.game + 0x14e84, f.units); f.put(f.game + 0x14e88, f.units + 7 * UNIT)
        for slot in range(8):
            u = f.units + slot * UNIT
            f.p.uc.mem_write(u, bytes([0xc3]) * UNIT)
            f.put(u + 0x130, 0); f.put(u + 2, slot, 'H'); f.put(u + 0xb8, records[slot // 4])
            f.p.uc.mem_write(u + 0xfd, bytes([slot // 4]))
        old = f.units + 1 * UNIT
        f.put(old, kind, 'H'); f.put(old + 0x130, ALIVE | 0x20 | (DYING if dying else 0))
        f.put(old + 0xb4, tp); f.put(old + 0x108, f.remaining, 'f'); f.put(old + 0x10c, 77, 'H')
        f.p.uc.mem_write(old + 0x114, bytes([act]))
        f.put(old + 0x68, 100 << 16); f.put(old + 0x6c, 20 << 16); f.put(old + 0x70, 200 << 16)
        f.put(old + 0xd8, 40.0, 'f'); f.p.uc.mem_write(old + 0x10e, bytes([5])); f.put(old + 4, HEAP + 0x3f0000)
        f.put(records[0] + 0xe8, 1, 'H')
        units1 = 0
        if limited:   # one of the type already in the new owner's pool
            u = f.units + 6 * UNIT
            f.put(u, kind, 'H'); f.put(u + 0x130, ALIVE); units1 += 1
        if capped:    # the new owner's count at the cap
            u = f.units + 7 * UNIT
            f.put(u, 2, 'H'); f.put(u + 0x130, ALIVE); units1 += 1
            f.put(f.game + 0x14e7c, units1, 'H')
        f.put(records[1] + 0xe8, units1, 'H')
        f.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = f.p.call(0x514da0, (old, records[0 if same else 1], 0))
        assert not error, error
        made = [e for e in f.log if e[0] == 'created']
        new = made[0][1] if made else 0
        names = [e[0] for e in f.log]
        if same or dying:
            assert not f.log and not f.draws, f.log
            return '0'
        assert names[0] == 'captured', names
        if not new:
            assert 'damage' not in names, names
            return '0'
        assert names.index('created') < names.index('damage'), names
        assert ('damage', 0, old, 30000, 6, 0) in f.log, f.log
        assert ('scenario+0x14', new) in f.log, f.log
        assert f.get(new + 0xd4) == 0, 'the trigger latch is not 0'
        copied = f.get(new + 0x114, 'B')
        assert copied == act, (copied, act)
        scripts = [e[1] for e in f.log if e[0] == 'script']
        state = 1 if flag else 0
        want = (['Activate'] if flag else []) + \
            (['Activate'] if act & 1 and not state else ['Deactivate'] if not act & 1 and state else [])
        assert scripts == want, (scripts, want)
        fresh = (f.get(new + 0xd8, 'f') == 0.0 and f.get(new + 0x10e, 'B') == 0 and f.get(new + 0x110, 'B') == 0 and
                 f.get(new + 0x111, 'B') == 0 and f.get(new + 0xcc) == 0 and f.get(new + 0x12a, 'B') == 0 and
                 f.get(new + 0xf8) == 0 and f.get(new + 0xfc, 'B') == 10 and f.get(new + 0x10c, 'H') == 77 and
                 f.get(new + 4) == 0)
        return '1 %s | %d %d' % (' '.join(str(d) for d in f.draws), act & 5, 1 if fresh else 0)


def activate_default():
    """The FBI parser's activatewhenbuilt (4c032b..4c035f): the TDF getter
    543190(key, default) is asked with default 0, and its answer's bit 0
    becomes UnitDef +0x260 & 0x40000. Run natively with the getter answering
    its default (a missing key) and answering 1."""
    p = Icd()
    unit_type, section = HEAP, HEAP + 0x1000
    keys = []
    answer = [None]

    def getter(uc, sp):
        key, default = struct.unpack('<II', uc.mem_read(sp, 8))
        keys.append((bytes(uc.mem_read(key, 24)).split(b'\0')[0].decode(), default))
        return 2, default if answer[0] is None else answer[0]
    p.hooks[0x543190] = getter
    from unicorn.x86_const import UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_ESP
    flags = []
    for given in (None, 1):
        answer[0] = given
        keys.clear()
        p.uc.mem_write(unit_type, bytes(0x300)); p.uc.mem_write(section, bytes(0x10))
        p.uc.mem_write(unit_type + 0x260, struct.pack('<I', 0x40000))
        p.uc.reg_write(UC_X86_REG_EBX, unit_type); p.uc.reg_write(UC_X86_REG_ESI, section)
        p.uc.reg_write(UC_X86_REG_ESP, 0x70000000 + 0x100000 - 0x2000)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(0x4c0327, 0x4c035f, count=40)
        assert keys == [('activatewhenbuilt', 0)], keys
        flags.append(struct.unpack('<I', p.uc.mem_read(unit_type + 0x260, 4))[0] & 0x40000)
    assert flags == [0, 0x40000], flags
    return 'activatewhenbuilt: default 0, so a type without it is not activated at creation'


def unfinished(f):
    """An unfinished unit (+0x108 = remaining work) changing sides. 514da0
    creates the new object finished (512060's sixth argument 1, 514ec6: the
    activatewhenbuilt Activate of 512301 runs) and then copies the old
    object's +0x108 and +0x10c over (514f8a..514fa1). Nothing pushes a
    mission on it: its mission list head (+0x60) is never written, so it has
    no GetBuilt (pushed only by the builders, 401f95 / 405a2b) and nothing
    decays it (World: Unit::siteGetBuilt, site_decay_test changedSides)."""
    for remaining in (0.25, 0.5, 1.0):
        f.remaining = remaining
        try:
            f.run(0, 0, 0, 0, 1, 0, 1, 0)
        finally:
            f.remaining = 0.0
        new = [e for e in f.log if e[0] == 'created'][0][1]
        assert f.get(new + 0x108, 'f') == remaining, (remaining, f.get(new + 0x108, 'f'))
        assert f.get(new + 0x10c, 'H') == 77
        scripts = [e[1] for e in f.log if e[0] == 'script']
        assert scripts == ['Activate', 'Deactivate'], scripts
        assert f.get(new + 0x60) == 0xc3c3c3c3 and f.get(new + 0x64) == 0xc3c3c3c3, \
            'a mission was pushed on the new object'
    return ('unfinished: the new object is created finished, takes the remaining work after, '
            'and gets no mission (no GetBuilt)')


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/owner_change_test'
    print(activate_default())
    print(unfinished(Fixture()))
    f = Fixture()
    rows, expected = [], []
    for same, dying, capped, limited, flag, spread, moving, act in itertools.product(
            (0, 1), (0, 1), (0, 1), (0, 1), (0, 1), (0, 1, 2, 4096), (0, 1), (0, 1, 4, 5)):
        rows.append('%d %d %d %d %d %d %d %d' % (same, dying, capped, limited, flag, spread, moving, act))
        expected.append(f.run(same, dying, capped, limited, flag, spread, moving, act))
    out = subprocess.run([binary, '--native'], input='\n'.join(rows) + '\n', text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(out) == len(expected), (len(out), len(expected))
    for row, got, want in zip(rows, out, expected):
        if got.strip() != want:
            raise AssertionError('%s\n  port   %s\n  native %s' % (row, got, want))
    made = sum(1 for e in expected if e != '0')
    print('PASS: %d changes of owner match retail 514da0 (%d made a new object): the capture event first, '
          'the creation draws, the activation copy, the fresh fields, the old object killed with kind 6'
          % (len(rows), made))


if __name__ == '__main__':
    main()
