#!/usr/bin/env python3
"""Melee against flyers: run retail's own routines on synthetic units.

A unit's flight layer is the low two bits of +0x130 (1 on the ground, 2 in the
air, 0 off the map; World's flightGroundMode, embarked() for 0). Each part is
run natively and checked against the model World follows since protocol 307:

  52b980  MeleeWeapon's unit in-range test: a target in layer 2 is never in
          range, layers 0, 1 and 3 are boxed (two 2x2 bodies 0..48 px apart).
  412620  the target picker, sweep and idle-scan variants: a melee slot takes
          layer-1 candidates only (a landed flyer too), a plain ranged slot
          takes a flyer in the air.
  4de530  kind 3 (attack): a walker whose weapon is melee gets ATTACK_MELEE
          whatever the target's layer.
  404690  Attack_Melee's first dispatch: 5 (removed) with no other call for a
          target in the air; a grounded or landed one gets the slot, its
          target and the chase circle.
  409a50  the hit response: an idle melee victim answers (4d7de0) a grounded
          or landed attacker, never one in the air.
  529dc0  the Titan's Hammer (aoe 31, edge 0.7) striking a ground unit with a
          flyer straight overhead: the flyer is hit only within aoe/2 of the
          struck unit's height.

Sub-calls that need the live game (the enemy scan 411ae0, damage 531e50, the
range and navigator calls) are answered by hooks; the checks themselves run
natively. Observation only: nothing from the binary goes into the engine.

    python3 tools/re/check_melee_air.py
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW
import check_splash_units as splash

MELEE, PLAIN = 0x5f3af8, 0x5f3710      # handler vtables (RTTI MeleeWeapon, WeaponType)
AIR, GROUND, LANDED = 'air', 'ground', 'landed flyer'


class Game:
    """One fresh image with a two-player world laid out on its heap."""

    def __init__(self):
        self.p = Icd()
        self.next = HEAP + 0x100000
        self.game = self.alloc(0x20000)
        self.put(0x62d55c, self.game)
        override = self.alloc(0x100)                 # [0x62d558] -> record; byte +7 unset
        self.put(override, override + 0x40)
        self.put(0x62d558, override)
        self.zeros = self.alloc(0x400)               # empty category bitsets
        self.calls = []

    def put(self, a, v, fmt='<I'):
        self.p.uc.mem_write(a, struct.pack(fmt, v))

    def alloc(self, n):
        a = self.next
        self.next += (n + 0xfff) & ~0xfff
        self.p.uc.mem_write(a, b'\0' * n)
        return a

    def player(self, index):
        rec = self.alloc(0x200)
        self.put(rec, 1)
        self.p.uc.mem_write(rec + 0xea, bytes([1]))   # a local human player
        self.p.uc.mem_write(rec + 0xeb, bytes([index]))
        self.p.uc.mem_write(rec + 0xe3, bytes([1]))
        return rec

    def weapon(self, vtable):
        w, handler = self.alloc(0x200), self.alloc(0x40)
        self.put(handler, vtable)
        self.put(w + 0x40, handler)
        self.put(w + 0x90, 300)
        return w

    def unit(self, uid, player, x, y, layer, weapon=None, flyer=False):
        kind, u = self.alloc(0x400), self.alloc(0x200)
        self.put(kind + 0x260, 0x8000 | (0x800 if flyer else 0))   # shootme, canfly
        self.put(kind + 0x264, 0x20)                                # canattack
        self.put(kind + 0x9a, self.zeros)
        for slot in range(3):
            self.put(kind + 0x8e + 4 * slot, self.zeros)
        self.p.uc.mem_write(kind + 0x226, struct.pack('<h', 190))
        self.p.uc.mem_write(kind + 0x232, struct.pack('<H', 500))
        self.p.uc.mem_write(u, struct.pack('<H', uid))
        self.put(u + 0xb4, kind)
        self.put(u + 0xb8, player)
        self.p.uc.mem_write(u + 0xfd, bytes(self.p.uc.mem_read(player + 0xeb, 1)))
        self.p.uc.mem_write(u + 0x68, struct.pack('<3i', x << 16, y << 16, 1000 << 16))
        self.p.uc.mem_write(u + 0x78, struct.pack('<2h', 2, 2))
        self.put(u + 0x130, 0x1000000 | 0x8000000 | 0x80000 | layer)   # alive, armed, fire at will
        self.put(u + 8, self.alloc(0x100))                              # a mover
        if weapon:
            self.put(kind + 0x1aa, weapon)
            self.put(u + 0xc, weapon)
        return u

    def hook(self, address, nargs, value, name=None):
        def answer(_uc, _sp):
            if name:
                self.calls.append(name)
            return nargs, value
        self.p.hooks[address] = answer

    def enemy(self, what, x=1100, uid=10, foe=None):
        layer = 2 if what == AIR else 1
        return self.unit(uid, foe, x, 150 if what == AIR else 0, layer, flyer=what != GROUND)


def in_range():
    g = Game()
    shooter, target, weapon = g.alloc(0x200), g.alloc(0x200), g.weapon(MELEE)
    answers = {}
    for layer in range(4):
        for gap in (0, 32, 40, 48):
            g.p.uc.mem_write(shooter + 0x68, struct.pack('<3i', 1000 << 16, 0, 1000 << 16))
            g.p.uc.mem_write(target + 0x68, struct.pack('<3i', (1000 + gap) << 16, 120 << 16, 1000 << 16))
            for body in (shooter, target):
                g.p.uc.mem_write(body + 0x78, struct.pack('<2h', 2, 2))
            g.put(shooter + 0x130, 0x1000001)
            g.put(target + 0x130, 0x1000000 | layer)
            value, error = g.p.call(0x52b980, (shooter, target, weapon), ecx=g.alloc(0x40))
            assert not error, error
            answers[layer, gap] = value & 0xff
    for (layer, gap), value in answers.items():
        want = 0 if layer == 2 or gap >= 48 else 1
        assert value == want, ('52b980', layer, gap, value)
    print('52b980: layers 0/1/3 in range in contact (0..40 px), layer 2 never: %d answers' % len(answers))


def picker():
    rows = 0
    for vtable, melee in ((MELEE, True), (PLAIN, False)):
        for sweep in (1, 0):
            for layout in ((AIR,), (GROUND,), (AIR, GROUND), (LANDED,)):
                g = Game()
                weapon = g.weapon(vtable)
                me = g.unit(1, g.player(0), 1000, 0, 1, weapon)
                foe = g.player(1)
                foes = [(what, g.unit(10 + i, foe, 1100, 150 if what == AIR else 0,
                                      2 if what == AIR else 1, flyer=what != GROUND))
                        for i, what in enumerate(layout)]
                listed = g.alloc(0x100)

                def scan(uc, sp, foes=foes, listed=listed):
                    out = struct.unpack('<5I', uc.mem_read(sp, 20))[4]
                    for k, (_, f) in enumerate(foes):
                        uc.mem_write(listed + 4 * k, struct.pack('<I', f))
                    uc.mem_write(out + 4, struct.pack('<3I', listed, listed + 4 * len(foes), listed + 0x100))
                    return 5, 0
                g.p.hooks[0x411ae0] = scan
                g.hook(0x531e50, 1, 500)        # damage
                g.hook(0x52adf0, 3, 0)          # not inside minrange
                g.hook(0x52ad60, 3, 1)          # a ranged slot: in range
                g.hook(0x4eba00, 0, 0)          # free
                g.hook(0x51b340, 2, 0)          # direction
                g.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
                value, error = g.p.call(0x412620, (me, 0, sweep))
                assert not error, error
                pick = next((what for what, f in foes if f == value), None)
                grounded = [what for what in layout if what != AIR]
                if melee:
                    assert pick == (grounded[0] if grounded else None), ('412620', melee, sweep, layout, pick)
                else:   # any candidate will do; a lone flyer in the air is taken
                    assert pick in layout, ('412620', melee, sweep, layout, pick)
                rows += 1
    print('412620: a melee slot never picks a flyer in the air (landed: picked); a ranged one does: %d cases' % rows)


def mission():
    for what in (AIR, GROUND, LANDED):
        g = Game()
        weapon = g.weapon(MELEE)
        me = g.unit(1, g.player(0), 1000, 0, 1, weapon)
        target = g.enemy(what, foe=g.player(1))
        names = []

        def lookup(uc, sp):
            name = struct.unpack('<I', uc.mem_read(sp, 4))[0]
            names.append(bytes(uc.mem_read(name, 24)).split(b'\0')[0].decode())
            this = uc.reg_read(UC_X86_REG_ECX)
            uc.mem_write(this, bytes([0x33]))
            return 1, this
        g.p.hooks[0x4d4bf0] = lookup
        g.hook(0x531e50, 1, 500)
        out = g.alloc(0x10)
        _, error = g.p.call(0x4de530, (out, 3, me, target, 0, 0))
        assert not error, error
        assert g.p.uc.mem_read(out, 1)[0] and names[-1] == 'ATTACK_MELEE', ('4de530', what, names)
    print('4de530 kind 3: ATTACK_MELEE against a flyer in the air, a ground unit and a landed flyer')

    for what in (AIR, GROUND, LANDED):
        g = Game()
        weapon = g.weapon(MELEE)
        me = g.unit(1, g.player(0), 1000, 0, 1, weapon)
        target = g.enemy(what, x=1300, foe=g.player(1))
        m = g.alloc(0x100)
        g.put(m + 0x16, target)
        for address, nargs, value, name in (
                (0x531e50, 1, 500, 'damage'), (0x52ad60, 3, 0, 'in range'), (0x51d1e0, 3, 0, '51d1e0'),
                (0x519b70, 1, 0, 'slots manual'), (0x51a700, 3, 0, 'slot target'),
                (0x4d4d40, 1, 0, 'stop'), (0x4d4da0, 2, 0, 'chase circle'), (0x4d4e80, 2, 0, 'rectangle'),
                (0x4d6a10, 2, 0, 'sleep'), (0x535e90, 2, 100, 'distance')):
            g.hook(address, nargs, value, name)
        value, error = g.p.call(0x404690, (me, m, 0))
        assert not error, error
        if what == AIR:
            assert value == 5 and not g.calls, ('404690', what, value, g.calls)
        else:
            assert value == 1 and g.calls[-3:] == ['slots manual', 'slot target', 'chase circle'], \
                ('404690', what, value, g.calls)
    print('404690: removed at once (5, no call) for a target in the air; '
          'a grounded or landed one is given to the slot and chased')


def response():
    for what in (AIR, GROUND, LANDED):
        g = Game()
        weapon = g.weapon(MELEE)
        me = g.unit(1, g.player(0), 1000, 0, 1, weapon)
        attacker = g.enemy(what, x=1150, foe=g.player(1))
        for address, nargs, value, name in (
                (0x519a90, 2, 0, None), (0x531e50, 1, 500, None), (0x52ad60, 3, 0, None),
                (0x52adf0, 3, 0, None), (0x4d7de0, 6, 1, 'attack'), (0x51a700, 3, 0, None),
                (0x51a9a0, 2, 0, None), (0x4d4f20, 1, 0x80, None), (0x4d4cc0, 0, 0, None),
                (0x535e90, 2, 150, None), (0x535cc0, 1, 0, None), (0x4d6a50, 2, 0, None)):
            g.hook(address, nargs, value, name)
        _, error = g.p.call(0x409a50, (attacker, me))
        assert not error, error
        assert (g.calls == ['attack']) == (what != AIR), ('409a50', what, g.calls)
    print('409a50: an idle melee victim answers a grounded or landed attacker, never one in the air')


def overhead():
    F = 65536
    edge = struct.unpack('<I', struct.pack('<f', 0.7))[0]
    impact = (200 * F + F // 2, 0, 200 * F + F // 2)
    box = lambda foot, top: ((-(foot << 19), 0, -(foot << 19)), ((foot << 19), top * F, (foot << 19)))
    seen = []
    for height in (0, 5, 10, 14, 15, 16, 20, 50, 150, 200):
        struck = (1, 1, impact, *box(2, 30))
        flyer = (2, 1, (impact[0], height * F, impact[2]), *box(3, 25))
        titan = (3, 1, (impact[0] + 32 * F, 0, impact[2]), *box(2, 30))
        out = splash.native(30, 30, 31, edge, impact, [struck, flyer, titan], {(12, 12): (1, 2), (14, 12): (3, 0)}, 3)
        hit = {int(h.split(':')[0]) for h in out.split()[1:]}
        assert 1 in hit and 3 not in hit and ((2 in hit) == (height < 15)), ('529dc0', height, out)
        seen.append(height)
    print('529dc0: the Hammer splashes a flyer overhead only within 15 px of the struck unit (%s px tried)' %
          ', '.join(map(str, seen)))


if __name__ == '__main__':
    in_range()
    picker()
    mission()
    response()
    overhead()
    print('PASS: melee never reaches a flyer in the air; a landed one is an ordinary target')
