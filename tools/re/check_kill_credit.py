#!/usr/bin/env python3
"""Who gets the credit for a hit and a kill in retail, natively.

Fourteen pieces, each run on synthetic units:

  * the hitter gate of the damage routine 51a140 -> 51a240: a hitter whose
    object lives (+0x130 & 0x1000000, a dying one too) is stored at +0xf8 and
    its player (+0xfd) at +0xfc and handed to the hit response 409a50; a
    freed object (a retired unit, or the old object of one that changed
    sides) and no hitter leave both as they were and hand 409a50 nothing.
  * the attack multiplier of 52a330: the shooter's +0xe0 only while it lives
    and is not dying (+0x130 & 0x1000).
  * the death weapon 52ac00: a shot with no shooter (+0x7c = 0) for the dying
    unit's player (+0x92 = +0xfd) at its position, through 529c10.
  * retirement 512ae0: the conditions' death event 523510 (its only caller)
    and the scenario's +0x18 (UnitDestroyed) come with it, not with the
    death 512610; the death weapon fires inside it while the unit's
    object still lives, and only for severity > 0 (+0x115) on a finished
    unit (+0x108 == 0); the object is freed after.
  * the mission Kill 513130: every unit this machine runs loses its shots
    (52ae30(u, 1)), then a unit whose Dying runs (+0x130 & 0x10000000)
    retires at once (512ae0) and every other one dies with kind 10 (512610).
  * deadly water 51e0d5..51e14c: once a second (tick % 30 == 0), a unit
    whose height's whole part (+0x6e) is at or below sea level (game
    +0x19ef8), not canhover (UnitDef +0x260 & 0x1000), takes the map's whole
    waterdamage (+0xd45, when +0xd41 is set) through 51a140 with no hitter
    and damage kind 13: no hit response, the last hitter kept.

  * a carried unit in deadly water: 4dad30 commits its carrier's attach
    point as its own +0x68 (51b3b0), and 51e0d5 then reads that height;
    being carried (+0xa8) exempts nothing.

  * self-destruct: the SelfDestruct mission 4017e0 in the background list
    4d85e0 runs every 30 ticks, draws rand(15) when its countdown is spent
    and hits that many ticks later with 51a140(u, u, 30000, 5, 0): the unit
    is its own last hitter and 409a50's attacker, and the update ends with
    512610(u, 5).

  * the commander rule: 512610 of a commander, or of a unit carrying a live
    one, calls 5131b0 for its player with CommanderDeath on, outside a
    campaign mission; 5131b0 kills that player's every unit (kind 10) and
    retires them at once.

  * a carrier's passengers: 512860 retires each at once with kind 8 and the
    carrier's hitter, before the carrier's own Killed.

  * the HP sample 51e07c..51e0cf (once a second: HP x 100 over maxdamage,
    unsigned, clamped to 0..100; the old one moves to +0x111) and 512610's
    severity (the packet's +9: by death kind, the HP word and the sample of
    a second before), against `retail_script_test --hp-sample` and
    `--severity`; and the order inside the update: a unit that is dying
    takes its sample before 512610 ends the update (51e36c), so on a sample
    tick the severity reads the sample just moved into +0x111.

  * the reload 530140 starts: the weapon's own word, whatever the unit's
    experience or `v F` multiplier (veterans do not fire faster).

  * mogriumbounty: a credited kill pays the victim type's bounty into the
    killer player's pool and lifetime total.

  * the CRT's Heal and Damage actions (4cb106, 4cb1b8): for each of the
    player's units (game +0x2478..+0x247c) of the type (or any, 0) whose
    origin cell is in the region (none, -1: no unit), 51a140(0, unit,
    amount, 12 / 13, 0) with the action's whole dword, so 51a140's
    damagemodifier and its no-hitter rule apply as to any hit.

  * damagemodifier: 51a140 scales a hit under 30000 on an activated unit
    (+0x114 & 2) by UnitDef +0x17e, heals excepted, against
    `retail_script_test --damage-modifier`.

The deadly-water rows are answered by `retail_script_test --deadly-water`
and `--carried-water`.

    python3 tools/re/check_kill_credit.py [build-o2/retail_script_test]
"""
import itertools
import struct
import subprocess
import sys

from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESI, UC_X86_REG_EBX, UC_X86_REG_EDI, \
    UC_X86_REG_ESP, UC_X86_REG_EBP

STACK_TOP = 0x70000000 + 0x100000 - 0x2000
ALIVE, DYING, DEATH_RUNS = 0x1000000, 0x1000, 0x10000000


class Fixture:
    def __init__(self):
        self.p = Icd()
        (self.game, self.units, self.victim_type, self.local, self.remote, self.shot, self.flags,
         self.map) = (HEAP + i * 0x20000 for i in range(8))
        self.calls = []
        self.put(0x62d55c, self.game)
        self.put(self.local, 1); self.put(self.local + 0xea, 2, 'B')
        self.put(self.remote, 1); self.put(self.remote + 0xea, 3, 'B')
        self.put(self.victim_type + 0x1be, 100)
        self.put(self.game + 0x14e84, self.units); self.put(self.game + 0x14e88, self.units + 7 * 0x138)

    def put(self, address, value, fmt='I'):
        self.p.uc.mem_write(address, struct.pack('<' + fmt, value))

    def get(self, address, fmt='I'):
        return struct.unpack('<' + fmt, self.p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    def record(self, name, nargs, answer=0):
        def hook(uc, sp):
            self.calls.append((name,) + struct.unpack('<%dI' % nargs, uc.mem_read(sp, 4 * nargs)))
            return nargs, answer
        self.p.hooks[int(name, 16)] = hook

    def unit(self, index, owner=None, flags=ALIVE, player=0):
        u = self.units + index * 0x138
        self.p.uc.mem_write(u, bytes(0x138))
        self.put(u + 2, index, 'H'); self.put(u + 0x130, flags); self.put(u + 0xb4, self.victim_type)
        self.put(u + 0xb8, owner or self.local); self.put(u + 0xfd, player, 'B'); self.put(u + 0x10c, 50, 'H')
        return u

    def call(self, address, args=()):
        self.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        result, error = self.p.call(address, args)
        assert not error, error
        return result


def hitter_gate():
    """51a140(hitter, victim, 10, kind 1, 0) against each kind of hitter."""
    f = Fixture()
    f.record('409a50', 3); f.record('56c640', 8)   # the hit response; HitByWeapon's start
    f.put(0x62d558, f.flags); f.put(f.flags + 0x18, f.flags + 0x100)   # no blood
    previous = f.unit(5, player=6)
    rows = []
    for name, flags in (('live', ALIVE), ('dying', ALIVE | DYING | DEATH_RUNS), ('freed', 0), ('none', None)):
        victim = f.unit(1, player=0)
        f.put(victim + 0xf8, previous); f.put(victim + 0xfc, 6, 'B')
        hitter = 0 if flags is None else f.unit(2, flags=flags, player=3)
        f.calls.clear()
        f.call(0x51a140, (hitter, victim, 10, 1, 0))
        stored = f.get(victim + 0xf8), f.get(victim + 0xfc, 'B')
        response = f.calls[0][1] if f.calls else None
        rows.append((name, stored, response, f.get(victim + 0x10c, 'H')))
        if flags is not None and flags & ALIVE:
            assert stored == (hitter, 3) and response == hitter, (name, stored, response)
        else:
            assert stored == (previous, 6) and response == 0, (name, stored, response)
        assert f.get(victim + 0x10c, 'H') == 40, name
    return ('hitter gate: a live or dying hitter is the last hitter (+0xf8, +0xfc) and 409a50\'s; '
            'a freed object or none keeps the old one and hands 409a50 nothing')


def attack_multiplier():
    """52a330(shot, victim, 1.0): the shooter's +0xe0 scales the damage."""
    f = Fixture()
    weapon = f.shot + 0x1000; damage_flags = f.flags
    f.put(0x60596c, 1, 'B'); f.put(0x62d558, damage_flags); f.put(damage_flags, damage_flags + 0x100)
    f.p.hooks[0x531e50] = lambda uc, sp: (1, 100)          # the weapon's damage against the victim
    f.p.hooks[0x5d4444] = lambda uc, sp: (0, 0)            # the CRT draw: no spread
    f.record('51a140', 5)
    answers = {}
    for name, flags in (('live', ALIVE), ('dying', ALIVE | DYING), ('freed', 0), ('none', None)):
        victim = f.unit(1); f.put(victim + 0xe4, 1.0, 'f')
        shooter = 0 if flags is None else f.unit(2, flags=flags)
        if shooter:
            f.put(shooter + 0xe0, 2.0, 'f')
        f.p.uc.mem_write(f.shot, bytes(0xdc)); f.put(f.shot, weapon); f.put(f.shot + 0x7c, shooter)
        f.calls.clear()
        f.call(0x52a330, (f.shot, victim, struct.unpack('<i', struct.pack('<f', 1.0))[0]))
        answers[name] = f.calls[0][3]
    base = answers['none']   # the CRT draw (hooked to 0) is the spread's low end
    assert answers == {'live': 2 * base, 'dying': base, 'freed': base, 'none': base}, answers
    return 'attack multiplier: +0xe0 scales a live shooter\'s damage only; dying, freed or none: x1'


def death_weapon_shot():
    """52ac00(unit, selfdestruct): the shot it hands 529c10."""
    f = Fixture()
    shots = []
    def impact(uc, sp):
        shot = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        shots.append(bytes(uc.mem_read(shot, 0xdc)))
        return 2, 0
    f.p.hooks[0x529c10] = impact
    u = f.unit(1, player=4)
    f.put(f.victim_type + 0x1b6, 0x1234); f.put(f.victim_type + 0x1ba, 0x5678)
    for i, value in enumerate((0x110000, 0x220000, 0x330000)):
        f.put(u + 0x68 + 4 * i, value)
    rows = []
    for selfdestruct in (0, 1):
        shots.clear()
        f.call(0x52ac00, (u, selfdestruct))
        shot = shots[0]
        weapon, x, y, z = struct.unpack_from('<4I', shot, 0)
        shooter = struct.unpack_from('<I', shot, 0x7c)[0]; player = shot[0x92]
        assert weapon == (0x5678 if selfdestruct else 0x1234) and (x, y, z) == (0x110000, 0x220000, 0x330000)
        assert shooter == 0 and player == 4, (shooter, player)
    return 'death weapon: 52ac00\'s shot has no shooter (+0x7c 0), the dying unit\'s player, its position'


def retirement():
    """512ae0 with its callees stubbed: when the death weapon fires."""
    f = Fixture()
    stubs = {'4c6750': 2, '4c6800': 2, '523510': 1, '50aa20': 1, '51b4f0': 5, '5066a0': 1,
             '512ee0': 5, '5ba5d0': 0, '4ee560': 1, '4dc7f0': 0, '4eba00': 0, '4c7190': 0}
    for name, nargs in stubs.items():
        f.record(name, nargs)
    scenario = f.flags; vtable = f.flags + 0x100; stub = f.flags + 0x200
    f.put(0x62da3c, scenario); f.put(scenario, vtable); f.put(vtable + 0x18, stub)
    f.p.uc.mem_write(stub, b'\xc2\x04\x00')                    # ret 4
    f.p.hooks[stub] = lambda uc, sp: (f.calls.append(('scenario+18',) + struct.unpack('<I', uc.mem_read(sp, 4)))
                                      or (1, 0))
    f.put(f.game + 0x175dc, f.map)
    alive_at_blast = []
    def blast(uc, sp):
        unit = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        alive_at_blast.append(f.get(unit + 0x130) & ALIVE)
        f.calls.append(('52ac00', unit))
        return 2, 0
    f.p.hooks[0x52ac00] = blast
    cases = []
    ordered = 0
    for severity, finished, suppressed, kind, corpse in itertools.product((0, 1, 60), (0, 1), (0, 1), (1, 3, 5, 7),
                                                                          (0, 1, 2)):
        u = f.unit(1, flags=ALIVE | DYING | DEATH_RUNS)
        f.put(u + 0x115, severity, 'B'); f.put(u + 0x116, kind << 4 | corpse, 'B')
        f.calls.clear()
        f.put(u + 0x108, 0.0 if finished else 0.5, 'f'); f.put(u + 0xfc, 10, 'B')
        f.put(f.game + 0x19f94, 2 if suppressed else 0)
        alive_at_blast.clear()
        f.call(0x512ae0, (u,))
        fired = bool(alive_at_blast)
        assert fired == (severity > 0 and finished and not suppressed), (severity, finished, suppressed, kind)
        assert all(alive_at_blast), 'the object was freed before its death weapon'
        assert not f.get(u + 0x130) & ALIVE, 'retirement left the object alive'
        # The corpse (512ee0, for a corpse kind in +0x116's low nibble) comes
        # after the death weapon: the blast never reaches its own corpse.
        names = [c[0] for c in f.calls]
        assert ('512ee0' in names) == (corpse > 0), (corpse, names)
        # The conditions' death event (523510, which only 512ae0 calls) and
        # the scenario's +0x18 (UnitDestroyed) come with the retirement.
        assert ('523510', u) in f.calls and ('scenario+18', u) in f.calls, f.calls
        assert names.index('523510') < names.index('scenario+18'), names
        if fired and corpse:
            assert names.index('52ac00') < names.index('512ee0'), names
            ordered += 1
        cases.append(fired)
    return ('retirement: 512ae0 sends the death to the conditions (523510) and the scenario (+0x18), '
            'fires the death weapon (%d of %d cases) while the object lives, for '
            'severity > 0 on a finished unit, then frees it; the corpse (512ee0) after the blast in all %d '
            'cases with both' % (sum(cases), len(cases), ordered))


def mission_kill():
    """513130 over a local live, a local dying, a local retiring, a freed and a remote unit."""
    f = Fixture()
    f.record('52ae30', 2); f.record('512610', 2); f.record('512ae0', 1)
    live = f.unit(1); dying = f.unit(2, flags=ALIVE | DYING); retiring = f.unit(3, flags=ALIVE | DYING | DEATH_RUNS)
    f.unit(4, flags=0); f.unit(5, owner=f.remote)
    for i in (0, 6):
        f.p.uc.mem_write(f.units + i * 0x138, bytes(0x138))
    f.call(0x513130)
    expected = [('52ae30', live, 1), ('512610', live, 10), ('52ae30', dying, 1), ('512610', dying, 10),
                ('52ae30', retiring, 1), ('512ae0', retiring)]
    assert f.calls == expected, f.calls
    return ('mission Kill: every local unit loses its shots; a retiring one retires at once, '
            'every other one dies with kind 10; freed and remote units are skipped')


def deadly_water(binary):
    """51e0d5..51e152 for every combination of its inputs, against the port."""
    f = Fixture()
    f.record('51a140', 5)
    from unicorn import UC_HOOK_CODE
    f.p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x51e357, end=0x51e357)   # no local owner
    unit_type = f.victim_type
    rows, expected = [], []
    for frame, height, sea, hover, enabled, amount, owner in itertools.product(
            (0, 1, 29, 30, 31, 60, 899, 900), (-3, 0, 4, 5, 6, 200), (0, 5, 255), (0, 1), (0, 1),
            (0, 1, 100, 30000), (1, 2, 3)):
        u = f.unit(1, owner={1: f.local, 2: f.local, 3: f.remote}[owner])
        f.put(f.local + 0xea, owner if owner != 3 else 1, 'B')
        y = ((height << 16) | 0x8000) & 0xffffffff
        f.put(u + 0x6c, y)
        f.put(unit_type + 0x260, 0x1000 if hover else 0)
        f.put(f.game + 0x19f44, frame); f.put(f.game + 0x19ef8, sea, 'B'); f.put(f.game + 0x175dc, f.map)
        f.put(f.map + 0xd41, enabled); f.put(f.map + 0xd45, amount)
        f.calls.clear()
        f.p.uc.reg_write(UC_X86_REG_ESI, u); f.p.uc.reg_write(UC_X86_REG_EBX, f.game)
        f.p.uc.reg_write(UC_X86_REG_EDI, 0)
        f.p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); f.p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
        f.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        f.p.uc.emu_start(0x51e0d5, 0x51e152, count=200)
        if f.calls:
            assert f.calls[0][1] == 0 and f.calls[0][2] == u and f.calls[0][4] == 13, f.calls
        if owner == 3:
            continue   # a remote player's unit: its own machine hurts it (lockstep runs every unit)
        rows.append('%d %d %d %d %d %d' % (frame, struct.unpack('<i', struct.pack('<I', y))[0], sea, hover,
                                          enabled, amount))
        expected.append('%d' % (f.calls[0][3] if f.calls else 0))
    output = subprocess.run([binary, '--deadly-water'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, got, want in zip(rows, actual, expected):
        if got != want:
            raise AssertionError('%s\n  port   %s\n  native %s' % (row, got, want))
    hurt = sum(1 for e in expected if e != '0')
    return 'deadly water: %d rows match (%d hurt), no hitter, kind 13' % (len(rows), hurt)


def carried_water(binary):
    """Deadly water on a carried unit: 4dad30 commits the carrier's attach
    point as the cargo's +0x68 (51b3b0), then 51e0d5 reads that +0x6e; being
    carried (+0xa8) exempts nothing. Natively chained, against the port."""
    import random
    from unicorn import UC_HOOK_CODE
    f = Fixture()
    f.record('51a140', 5)
    committed = []

    def commit(uc, sp):
        unit, x, y, z, _flags = struct.unpack('<Iiiii', uc.mem_read(sp, 20))
        committed.append((unit, x, y, z))
        return 5, 0
    f.p.hooks[0x51b3b0] = commit
    stop = [0]
    f.p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: (stop.__setitem__(0, 1), uc.emu_stop()),
                    begin=0x4dadd7, end=0x4dadd7)
    host, mover, kind = HEAP + 0x300000, HEAP + 0x301000, HEAP + 0x302000
    rng = random.Random(0x51e13f)
    rows, expected = [], []
    for case in range(600):
        sea = rng.choice((0, 20, 50, 128, 255))
        hy = rng.choice((sea - 1, sea, sea + 1, sea - 20, sea + 60, rng.randrange(-40, 300)))
        y = ((hy << 16) | rng.randrange(0x10000)) & 0xffffffff
        floater, waterline = rng.randrange(2), rng.choice((0, 3, 12, 40))
        amount = rng.choice((1, 100, 30000))
        tick = rng.choice((0, 30, 31, 900))
        cargo = f.unit(1)
        p = f.p
        p.uc.mem_write(host, bytes(0x200)); p.uc.mem_write(mover, bytes(0x100)); p.uc.mem_write(kind, bytes(0x300))
        f.put(host + 0x68, 4096 << 16); f.put(host + 0x6c, y); f.put(host + 0x70, 2048 << 16)
        f.put(cargo + 0xa8, host); f.put(cargo + 0xb4, kind); f.put(cargo + 0x112, 0xff, 'B')
        f.put(kind + 0x260, 0x80000 if floater else 0); f.put(kind + 0x248, waterline, 'B')
        f.put(f.game + 0x19ef8, sea, 'B'); f.put(f.game + 0x19f44, tick); f.put(f.game + 0x175dc, f.map)
        f.put(f.map + 0xd41, 1); f.put(f.map + 0xd45, amount)
        committed.clear(); stop[0] = 0
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x4dad30, (cargo,), ecx=mover, allow_early_stop=True)
        assert not error and stop[0] and len(committed) == 1 and committed[0][0] == cargo, (error, committed)
        _, cx, cy, cz = committed[0]
        f.put(cargo + 0x68, cx & 0xffffffff); f.put(cargo + 0x6c, cy & 0xffffffff); f.put(cargo + 0x70, cz & 0xffffffff)
        f.calls.clear()
        p.uc.reg_write(UC_X86_REG_ESI, cargo); p.uc.reg_write(UC_X86_REG_EBX, f.game)
        p.uc.reg_write(UC_X86_REG_EDI, 0)
        p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
        p.uc.emu_start(0x51e0d5, 0x51e152, count=200)
        assert f.get(cargo + 0xa8) == host
        rows.append('%d %d %d %d %d %d' % (struct.unpack('<i', struct.pack('<I', y))[0], floater, waterline,
                                           sea, amount, tick))
        expected.append('%d' % (f.calls[0][3] if f.calls else 0))
    output = subprocess.run([binary, '--carried-water'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, got, want in zip(rows, actual, expected):
        if got != want:
            raise AssertionError('%s\n  port   %s\n  native %s' % (row, got, want))
    hurt = sum(1 for e in expected if e != '0')
    return 'carried in deadly water: %d rows match (%d hurt at the carrier\'s attach point)' % (len(rows), hurt)


def water_hit():
    """The kind-13 hit itself: whole HP, no hit response, the last hitter kept."""
    f = Fixture()
    f.record('409a50', 3)
    v = f.unit(1); hitter = f.unit(2, player=5)
    f.put(v + 0xf8, hitter); f.put(v + 0xfc, 5, 'B')
    f.call(0x51a140, (0, v, 30, 13, 0))
    assert f.get(v + 0x10c, 'H') == 20 and not f.calls and f.get(v + 0xf8) == hitter
    f.call(0x51a140, (0, v, 30, 13, 0))
    assert f.get(v + 0x10c, 'h') == -10 and f.get(v + 0x130) & DYING and f.get(v + 0x10f, 'B') == 13
    return 'water hit: whole HP off, no 409a50, last hitter kept, kind 13 on the lethal one'


def water_death():
    """51e13f..51e371: a lethal water hit's 512610 runs at the end of the same
    update, after the missions (4d8450, skipped for a dying unit), the mover
    (4dc800) and the surface (51b2a0); a hit that leaves HP above zero runs
    none. Natively: 51a140 itself, the rest of the update's calls recorded."""
    from unicorn import UC_HOOK_CODE
    f = Fixture()
    for name, n in (('429d90', 4), ('401160', 1), ('4d8450', 1), ('4d85e0', 1), ('4dc800', 1),
                    ('51b2a0', 1), ('5195e0', 1), ('512060', 9), ('512610', 2), ('409a50', 3)):
        f.record(name, n)
    f.p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x51e371, end=0x51e371)
    seen = []
    for hp, amount in ((50, 100), (50, 50), (50, 49), (300, 100)):
        u = f.unit(1); f.put(u + 0x10c, hp, 'H'); f.put(u + 8, f.units + 0x1000)   # a mover
        f.put(f.victim_type + 0x260, 0); f.put(f.victim_type + 0x268, 0); f.put(f.victim_type + 0x21e, 0.0, 'f')
        f.put(u + 0x6c, (2 << 16) | 0x8000)
        f.put(f.game + 0x19f44, 30); f.put(f.game + 0x19ef8, 5, 'B'); f.put(f.game + 0x175dc, f.map)
        f.put(f.map + 0xd41, 1); f.put(f.map + 0xd45, amount)
        f.calls.clear()
        f.p.uc.reg_write(UC_X86_REG_ESI, u); f.p.uc.reg_write(UC_X86_REG_EBX, f.game)
        f.p.uc.reg_write(UC_X86_REG_EDI, 0)
        f.p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); f.p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
        f.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        f.p.uc.emu_start(0x51e0d5, 0x51e371, count=2000)
        names = [c[0] for c in f.calls]
        lethal = hp <= amount
        assert bool(f.get(u + 0x130) & DYING) == lethal, (hp, amount)
        if lethal:
            assert names[-1] == '512610' and f.calls[-1][1:] == (u, 13), f.calls
            assert names.index('4dc800') < names.index('51b2a0') < names.index('512610'), names
            assert '4d8450' in names, names          # called; it returns at once for a dying unit (4d8475)
        else:
            assert '512610' not in names, names
        seen.append(lethal)
    return ('water death: a lethal water hit\'s 512610 runs at the end of the same update (kind 13); '
            '%d lethal, %d not' % (seen.count(True), seen.count(False)))


SELFDESTRUCT = 11   # row 0x5eb63b of the mission definitions at 0x5eb528 (4d4cc0 indexes it)


def self_destruct():
    """The SelfDestruct mission 4017e0 run by the background list 4d85e0 once
    per update, natively both, for countdowns 0..3 and draws 0..14: the run
    every 30 ticks, rand(15) on the run that finds the countdown spent, the
    hit 51a140(u, u, 30000, 5, 0) that many ticks later (a draw of 0 in the
    same update: 4d85e0 takes the list from its head again), the mission
    removed (4d6ad0). A countdown of 0 hits on the first run, no draw."""
    rows = 0
    for countdown in range(4):
        for r in (0, 1, 7, 14):
            f = Fixture()
            f.put(0x62db84, 0x5eb528)
            draws, hits, tick = [], [], [0]

            def rand(uc, sp, r=r):
                draws.append((tick[0], struct.unpack('<I', uc.mem_read(sp, 4))[0]))
                return 1, r

            def hit(uc, sp):
                hits.append((tick[0],) + struct.unpack('<5I', uc.mem_read(sp, 20)))
                return 5, 0

            def remove(uc, sp):
                unit, mission = struct.unpack('<2I', uc.mem_read(sp, 8))
                hits.append(('removed', tick[0]))
                f.put(unit + 0x64, f.get(mission + 0x66))
                return 2, 0
            f.p.hooks[0x535cc0] = rand; f.p.hooks[0x51a140] = hit; f.p.hooks[0x4d6ad0] = remove
            u = f.unit(1)
            f.put(f.victim_type + 0x264, countdown << 21)
            m = f.units + 0x4000
            f.p.uc.mem_write(m, bytes(0x80))
            f.put(m + 4, SELFDESTRUCT, 'B'); f.put(m + 0xe, u); f.put(u + 0x64, m)
            for k in range(120):
                tick[0] = k
                f.put(f.game + 0x19f44, 1000 + k)
                f.call(0x4d85e0, (u,))
                if hits:
                    break
            at = 30 * countdown + (r if countdown else 0)
            assert draws == ([(30 * countdown, 15)] if countdown else []), (countdown, r, draws)
            assert hits == [(at, u, u, 30000, 5, 0), ('removed', at)], (countdown, r, hits)
            rows += 1
    return 'self-destruct: %d timelines, rand(15) when the countdown is spent, the hit that many ticks on' % rows


def self_destruct_hit():
    """51a140(u, u, 30000, 5, 0): the unit is its own last hitter and 409a50's
    attacker (before the HP drop); 16-bit HP, so only more than 30000 lives."""
    f = Fixture()
    f.record('409a50', 3); f.record('56c640', 8)
    f.put(0x62d558, f.flags); f.put(f.flags + 0x18, f.flags + 0x100)
    for hp, left in ((50, -29950), (29999, -1), (30000, 0), (30001, 1)):
        u = f.unit(1, player=4)
        f.put(u + 0x10c, hp, 'H'); f.put(u + 0xc0, f.units + 0x6000)
        f.calls.clear()
        f.call(0x51a140, (u, u, 30000, 5, 0))
        assert f.get(u + 0x10c, 'h') == left and bool(f.get(u + 0x130) & DYING) == (left <= 0), hp
        assert (f.get(u + 0xf8), f.get(u + 0xfc, 'B'), f.get(u + 0x10f, 'B')) == (u, 4, 5), hp
        assert f.calls == [('409a50', u, u, 30000)], f.calls
    return 'self-destruct hit: its own last hitter and 409a50 attacker; dying at 30000 HP or less'


def self_destruct_death():
    """51e0d5..51e371 with an expired SelfDestruct in the background list
    (4d85e0, native): the missions 4d8450 first, then the hit, the mover and
    surface, and the update ends with 512610(u, 5)."""
    from unicorn import UC_HOOK_CODE
    f = Fixture()
    f.put(0x62db84, 0x5eb528)
    for name, n in (('429d90', 4), ('401160', 1), ('4d8450', 1), ('4dc800', 1),
                    ('51b2a0', 1), ('5195e0', 1), ('512060', 9), ('512610', 2), ('409a50', 3), ('56c640', 8)):
        f.record(name, n)

    def remove(uc, sp):
        unit, mission = struct.unpack('<2I', uc.mem_read(sp, 8))
        f.calls.append(('4d6ad0', unit, mission))
        f.put(unit + 0x64, f.get(mission + 0x66))
        return 2, 0
    f.p.hooks[0x4d6ad0] = remove
    f.put(0x62d558, f.flags); f.put(f.flags + 0x18, f.flags + 0x100)
    f.p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x51e371, end=0x51e371)
    u = f.unit(1, player=2); f.put(u + 0x10c, 50, 'H'); f.put(u + 8, f.units + 0x1000)
    f.put(u + 0xc0, f.units + 0x6000)
    f.put(f.victim_type + 0x260, 0); f.put(f.victim_type + 0x268, 0); f.put(f.victim_type + 0x21e, 0.0, 'f')
    f.put(f.victim_type + 0x264, 2 << 21)
    f.put(u + 0x6c, (2 << 16) | 0x8000)
    f.put(f.game + 0x19f44, 31); f.put(f.game + 0x175dc, f.map); f.put(f.map + 0xd41, 0)
    m = f.units + 0x4000
    f.p.uc.mem_write(m, bytes(0x80))
    f.put(m + 4, SELFDESTRUCT, 'B'); f.put(m + 0xe, u); f.put(m + 0x4e, 1); f.put(m + 0x52, 0xf0000000)
    f.put(u + 0x64, m)
    f.p.uc.reg_write(UC_X86_REG_ESI, u); f.p.uc.reg_write(UC_X86_REG_EBX, f.game)
    f.p.uc.reg_write(UC_X86_REG_EDI, 0)
    f.p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); f.p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
    f.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    f.p.uc.emu_start(0x51e0d5, 0x51e371, count=4000)
    names = [c[0] for c in f.calls]
    assert names == ['401160', '4d8450', '409a50', '4d6ad0', '4dc800', '51b2a0', '512610'], names
    assert f.calls[-1] == ('512610', u, 5), f.calls
    return 'self-destruct death: after the missions, the hit, the mover and surface; 512610(u, 5) ends the update'


def commander():
    """512610's commander rule (51280d..512852) and 5131b0, natively. The rule
    calls 5131b0 with the dying unit's player when CommanderDeath is on
    ([[62d558]+8]+8 == 1, the 'CommanderDeath' setting), the owner is this
    machine's (+0xea 1 or 2), the map mode ([game+0x175dc]+0) is not 1 (a
    campaign mission), and the unit is a commander (UnitDef +0x264 bit 18) or
    carries a live one (the +0xac/+0xb0 chain, walked until an entry that does
    not live). 5131b0, for a player with units (record +0xe8): every unit
    whose object lives gets 512610(u, 10) and, still living, 512ae0; one whose
    Dying runs (+0x130 & 0x10000000) is retired at once."""
    def rule(commander, option=1, mode=2, remote=False, kind=1, cargo=()):
        f = Fixture()
        opts, inner, scen = f.flags, f.flags + 0x100, f.flags + 0x200
        f.put(0x62d558, opts); f.put(opts + 8, inner); f.put(inner + 8, option); f.put(opts + 0x18, opts + 0x300)
        f.put(f.game + 0x175dc, scen); f.put(scen, mode)
        for name, n in (('5131b0', 1), ('512860', 2), ('4e8030', 3), ('56c720', 5)):
            f.record(name, n)
        f.p.hooks[0x4e6860] = lambda uc, sp: (1, 0)
        plain = f.victim_type + 0x400
        f.put(f.victim_type + 0x264, 0x40000); f.put(plain + 0x264, 0)
        f.put(f.victim_type + 0x1be, 100); f.put(plain + 0x1be, 100)
        u = f.unit(1, player=3)
        f.put(u + 0xb4, f.victim_type if commander else plain)
        f.put(u + 0x10c, 0, 'H'); f.put(u + 0x108, 0.0, 'f'); f.put(u + 0xbc, f.units + 0x6000)
        link = u + 0xac
        for i, (boss, flags) in enumerate(cargo):
            c = f.unit(2 + i, flags=flags, player=4)
            f.put(c + 0xb4, f.victim_type if boss else plain)
            f.put(link, c); link = c + 0xb0
        if remote:
            f.put(u + 0xb8, f.remote)
        f.calls.clear()
        f.call(0x512610, (u, kind))
        return [c[1] & 0xff for c in f.calls if c[0] == '5131b0']
    assert rule(True) == [3] and rule(True, kind=10) == [3], 'a commander'
    assert rule(True, option=0) == [] and rule(True, mode=1) == [] and rule(True, remote=True) == [], 'the gates'
    assert rule(False) == [] and rule(False, cargo=((False, ALIVE),)) == [], 'no commander'
    assert rule(False, cargo=((True, ALIVE),)) == [3], 'a carried commander: the carrier\'s player'
    assert rule(False, cargo=((False, ALIVE), (True, ALIVE))) == [3], 'a carried commander second'
    assert rule(False, cargo=((False, 0), (True, ALIVE))) == [], 'the walk stops at a freed entry'

    f = Fixture()
    f.record('512ae0', 1); f.record('52ac00', 2); f.record('512610', 2)
    pool = f.units + 0x138
    rec = f.game + 3 * 0x110 + 0x2404
    f.put(rec, 1)
    f.put(f.game + 3 * 0x110 + 0x2478, pool); f.put(f.game + 3 * 0x110 + 0x247c, pool + 3 * 0x138)
    for count, want in ((4, [('512610', 0, 10), ('512ae0', 0), ('512ae0', 1), ('512610', 3, 10), ('512ae0', 3)]),
                        (0, [])):
        f.put(rec + 0xe8, count, 'H')
        for i, state in enumerate((ALIVE, ALIVE | DEATH_RUNS, 0, ALIVE)):
            f.unit(1 + i, flags=state, player=3)
        f.calls.clear()
        f.call(0x5131b0, (3,))
        got = [(c[0], (c[1] - pool) // 0x138) + tuple(c[2:]) for c in f.calls]
        assert got == want, (count, got)
    return ('commander: a commander\'s death, or its carrier\'s, ends with 5131b0 for that player '
            '(CommanderDeath on, a local owner, no campaign); 5131b0 kills every live unit kind 10 '
            '(512610 keeps the last hitter: see cargo) and retires a dying one at once')


def cargo():
    """512860 on a dying carrier with two passengers (one another player's),
    natively with its recursion: before the carrier's own Killed, each
    passenger in chain order gets the carrier's packet as kind 8, severity 0:
    +0xf8 the carrier's hitter while it lives (else 0), +0xfc its player
    either way, +0x116 0x80 (kind 8, no corpse), no Killed, and retires at
    once (512ae0, which detaches it); no 51a140."""
    f = Fixture()
    for name, n in (('4d6a50', 2), ('50b4f0', 2), ('52ae30', 2), ('519a60', 1), ('56c640', 8), ('51a140', 5)):
        f.record(name, n)
    f.p.hooks[0x4e67c0] = lambda uc, sp: (1, struct.unpack('<I', uc.mem_read(sp, 4))[0] & 0xff)
    f.p.hooks[0x56c4a0] = lambda uc, sp: (1, 0xffffffff)   # no Dying

    def retire(uc, sp):
        u = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        f.calls.append(('512ae0', u, f.get(u + 0xf8), f.get(u + 0xfc, 'B'), f.get(u + 0x116, 'B'),
                        f.get(u + 0x115, 'b')))
        carrier = f.get(u + 0xa8)
        if carrier:   # 512b57: 51b4f0 detaches it
            f.put(carrier + 0xac, f.get(u + 0xb0)); f.put(u + 0xa8, 0)
        return 1, 0
    f.p.hooks[0x512ae0] = retire
    for lives in (True, False):
        t = f.unit(1, player=2); k = f.unit(2, flags=ALIVE if lives else 0, player=5)
        a = f.unit(3, player=2); b = f.unit(4, player=6)
        for x in (t, a, b):
            f.put(x + 0xc0, f.units + 0x6000 + (x & 0xfff))
        f.put(t + 0xac, a); f.put(a + 0xb0, b); f.put(a + 0xa8, t); f.put(b + 0xa8, t)
        pkt = f.units + 0x7000
        f.p.uc.mem_write(pkt, struct.pack('<BHIHbB', 9, 1, 5, 2, 40, 0x11))   # the carrier's packet: kind 1
        f.calls.clear()
        f.call(0x512860, (pkt, 0))
        retired = [c[1:] for c in f.calls if c[0] == '512ae0']
        hitter = k if lives else 0
        assert retired == [(a, hitter, 5, 0x80, 0), (b, hitter, 5, 0x80, 0), (t, hitter, 5, 0x11, 40)], retired
        names = [c[0] for c in f.calls]
        assert '51a140' not in names and names.index('56c640') > names.index('512ae0'), names
    # The old object of a carrier that changed sides (514da0's 51a140 kind 6):
    # its 512610 sends the packet through 512860 too, so its passengers go.
    f.record('4e8030', 3); f.p.hooks[0x4e6860] = lambda uc, sp: (1, struct.unpack('<I', uc.mem_read(sp, 4))[0])
    opts = f.flags
    f.put(0x62d558, opts); f.put(opts + 8, opts + 0x100); f.put(opts + 0x108, 0); f.put(opts + 0x18, opts + 0x300)
    t = f.unit(1, player=2); k = f.unit(2, player=5); a = f.unit(3, player=2)
    for x in (t, a):
        f.put(x + 0xc0, f.units + 0x6000 + (x & 0xfff))
    f.put(t + 0xac, a); f.put(a + 0xa8, t); f.put(t + 0xf8, k); f.put(t + 0xfc, 5, 'B')
    f.calls.clear()
    f.call(0x512610, (t, 6))
    retired = [c[1:] for c in f.calls if c[0] == '512ae0']
    assert retired[0] == (a, k, 5, 0x80, 0) and retired[-1][0] == t and retired[-1][3] == 0x60, retired
    # 5131b0's 512610(u, 10) (the commander rule, a CRT defeat): the unit
    # retires with its last hitter while that lives, its player either way.
    for lives in (True, False):
        u = f.unit(1, player=2); k = f.unit(2, flags=ALIVE if lives else 0, player=5)
        f.put(u + 0xc0, f.units + 0x6000 + (u & 0xfff)); f.put(u + 0xf8, k); f.put(u + 0xfc, 5, 'B')
        f.calls.clear()
        f.call(0x512610, (u, 10))
        retired = [c[1:] for c in f.calls if c[0] == '512ae0']
        assert retired == [(u, k if lives else 0, 5, 0xa0, 0)], retired
    return ('cargo: a carrier\'s passengers die before its Killed, kind 8 and severity 0, with its hitter '
            '(while it lives) and player, and retire at once; so too when 512610 ends the old object of a '
            'carrier that changed sides (kind 6)')


def hp_sample(binary):
    """51e07c..51e0d5 natively for HP words and maxdamages, against the port."""
    import random
    f = Fixture()
    rng = random.Random(9)
    cases = [(hp, m) for hp in (-32768, -100, -1, 0, 1, 52, 53, 59, 99, 100, 101, 32767) for m in (1, 2, 3, 100, 651, 32767)]
    cases += [(rng.randint(-32768, 32767), rng.randint(1, 32767)) for _ in range(3000)]
    cases += [(hp, m) for m in (100, 200, 300, 700, 1300) for hp in range(0, m + 1, max(1, m // 100))]
    rows, expected = [], []
    for frame in (0, 30, 31):
        for hp, maximum in (cases if frame == 0 else cases[:40]):
            u = f.unit(1)
            f.put(u + 0x10c, hp & 0xffff, 'H'); f.put(f.victim_type + 0x1be, maximum)
            f.put(u + 0x110, 77, 'B'); f.put(u + 0x111, 66, 'B')
            f.put(f.game + 0x19f44, frame)
            f.p.uc.reg_write(UC_X86_REG_ESI, u); f.p.uc.reg_write(UC_X86_REG_EBX, f.game)
            f.p.uc.reg_write(UC_X86_REG_EDI, 0)
            f.p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); f.p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
            f.p.uc.emu_start(0x51e07c, 0x51e0d5, count=200)
            now, before = f.get(u + 0x110, 'B'), f.get(u + 0x111, 'B')
            if frame % 30:
                assert (now, before) == (77, 66), (frame, hp, maximum)
                continue
            assert before == 77, (hp, maximum)
            rows.append('%d %d' % (hp, maximum)); expected.append('%d' % now)
    output = subprocess.run([binary, '--hp-sample'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, got, want in zip(rows, actual, expected):
        if got != want:
            raise AssertionError('hp sample %s\n  port   %s\n  native %s' % (row, got, want))
    return 'hp sample: %d rows match; the old sample moves to +0x111; no sample off the second' % len(rows)


def severity(binary):
    """512610(u, kind) natively: the severity it puts in the packet (+9) and
    whether it runs Killed, against the port."""
    f = Fixture()
    opts, inner = f.flags, f.flags + 0x100
    f.put(0x62d558, opts); f.put(opts + 8, inner); f.put(inner + 8, 0); f.put(opts + 0x18, opts + 0x300)
    f.put(f.game + 0x175dc, f.flags + 0x200)
    packets = []
    def death_packet(uc, sp):
        packet = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        packets.append(bytes(uc.mem_read(packet, 11)))
        return 2, 0
    f.p.hooks[0x512860] = death_packet
    for name, n in (('5131b0', 1), ('4e8030', 3), ('56c720', 5)):
        f.record(name, n)
    f.p.hooks[0x4e6860] = lambda uc, sp: (1, 0)
    rows, expected, killed = [], [], 0
    for kind in range(16):
        for hp in (-32768, -3000, -651, -100, -99, -50, -1, 0, 1, 30):
            for maximum in (1, 3, 100, 651, 32767):
                for previous in (0, 1, 50, 53, 99, 100, 255):
                    u = f.unit(1, player=2)
                    f.put(u + 0x10c, hp & 0xffff, 'H'); f.put(u + 0x111, previous, 'B')
                    f.put(u + 0x108, 0.0, 'f'); f.put(f.victim_type + 0x1be, maximum)
                    f.put(u + 0xbc, f.units + 0x6000)
                    packets.clear(); f.calls.clear()
                    f.call(0x512610, (u, kind))
                    assert len(packets) == 1 and packets[0][10] >> 4 == kind, (kind, packets)
                    ran = any(c[0] == '56c720' for c in f.calls)
                    assert ran == (kind in (1, 2, 3, 4, 12, 13) and hp <= 0), (kind, hp, ran)
                    killed += ran
                    rows.append('%d %d %d %d' % (kind, hp, maximum, previous))
                    expected.append('%d' % packets[0][9])
    output = subprocess.run([binary, '--severity'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for row, got, want in zip(rows, actual, expected):
        if got != want:
            raise AssertionError('severity %s\n  port   %s\n  native %s' % (row, got, want))
    return 'severity: %d rows match (Killed in %d: kinds 1-4, 12 and 13 at HP 0 or less)' % (len(rows), killed)


def death_sample():
    """51e01f..51e371 natively for a unit already dying when its update runs:
    the sample (51e07c) comes before the update's 512610 (51e36c), so on a
    sample tick 512610 finds the sample of a second before in +0x111."""
    from unicorn import UC_HOOK_CODE
    f = Fixture()
    for name, n in (('429d90', 4), ('401160', 1), ('4d8450', 1), ('4d85e0', 1), ('4dc800', 1),
                    ('51b2a0', 1), ('5195e0', 1), ('512060', 9), ('409a50', 3)):
        f.record(name, n)
    seen = []
    def death(uc, sp):
        unit, kind = struct.unpack('<2I', uc.mem_read(sp, 8))
        seen.append((kind, f.get(unit + 0x111, 'B'), f.get(unit + 0x110, 'B')))
        return 2, 0
    f.p.hooks[0x512610] = death
    f.p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x51e371, end=0x51e371)
    rows = []
    for frame in (0, 1, 29, 30, 31, 60):
        u = f.unit(1, flags=ALIVE | DYING); f.put(u + 8, f.units + 0x1000)
        f.put(u + 0x10c, -20 & 0xffff, 'H'); f.put(u + 0x10f, 1, 'B')
        f.put(u + 0x110, 50, 'B'); f.put(u + 0x111, 100, 'B')
        f.put(f.victim_type + 0x260, 0); f.put(f.victim_type + 0x268, 0); f.put(f.victim_type + 0x21e, 0.0, 'f')
        f.put(u + 0x6c, (2 << 16) | 0x8000)
        f.put(f.game + 0x19f44, frame); f.put(f.game + 0x175dc, f.map); f.put(f.map + 0xd41, 0)
        seen.clear()
        f.p.uc.reg_write(UC_X86_REG_ESI, u); f.p.uc.reg_write(UC_X86_REG_EBX, f.game)
        f.p.uc.reg_write(UC_X86_REG_EDI, 0)
        f.p.uc.reg_write(UC_X86_REG_ESP, STACK_TOP); f.p.uc.reg_write(UC_X86_REG_EBP, STACK_TOP + 0x100)
        f.p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        f.p.uc.emu_start(0x51e01f, 0x51e371, count=4000)
        assert len(seen) == 1 and seen[0][0] == 1, (frame, seen)
        want = (50, 100) if frame % 30 == 0 else (100, 50)
        assert seen[0][1:] == want, (frame, seen)
        rows.append(frame)
    return ('death sample: a dying unit\'s update takes its sample before 512610; on a sample tick '
            '+0x111 holds the sample of a second before (frames %s)' % rows)


def veteran_reload():
    """530140(unit, slot) natively: the reload it starts is the weapon's
    word (+0x9c) with the CRT spread, whatever the unit's experience (+0xf0)
    or `v F` multiplier (+0xf4); 52ae90 counts it down one a tick."""
    f = Fixture()
    f.record('56c640', 8); f.record('4ea560', 2)
    weapon, slot = f.shot, f.shot + 0x200
    f.put(f.victim_type + 0x1c2, 100); f.put(f.victim_type + 0x268, 0)
    seen = {}
    for nominal in (1, 30, 90, 600):
        for roll in (0, 16384, 32767):
            f.p.hooks[0x5d4444] = lambda uc, sp, r=roll: (0, r)
            for xp, vf in ((0, 0.0), (1000, 0.0), (100000, 0.0), (0, 2.0)):
                u = f.unit(1); f.put(u + 0xf0, xp); f.put(u + 0xf4, vf, 'f'); f.put(u + 0xbc, f.units + 0x6000)
                f.p.uc.mem_write(weapon, bytes(0x100)); f.put(weapon + 0x9c, nominal, 'H')
                f.p.uc.mem_write(slot, bytes(0x1c)); f.put(slot, weapon)
                f.call(0x530140, (u, slot))
                seen.setdefault((nominal, roll), set()).add(f.get(slot + 0x14, 'H'))
    assert all(len(v) == 1 for v in seen.values()), seen
    return 'veteran reload: 530140 starts the same reload at every rank (%d nominal/roll pairs)' % len(seen)


def damage_modifier(binary):
    """51a140(0, u, damage, kind, 0) natively: the damage it takes off the HP
    word with the activation byte (+0x114) and the type's damagemodifier
    (UnitDef +0x17e, 16.16), against the port."""
    import random
    f = Fixture()
    f.record('409a50', 3); f.record('56c640', 8); f.record('51a660', 2)
    f.put(0x62d558, f.flags); f.put(f.flags + 0x18, f.flags + 0x100)
    rng = random.Random(5)
    rows, expected = [], []
    for kind in (1, 2, 3, 12, 13):
        for byte in (0, 1, 2, 3, 0x12):
            for modifier in (0, 0x8000, 0x10000, 0x18000, 0x30000, -0x10000):
                for damage in (0, 1, 7, 100, 999, 29999, 30000, 40000) + tuple(rng.randint(0, 30000) for _ in range(3)):
                    u = f.unit(1); f.put(u + 0x10c, 30000, 'H'); f.put(u + 0xc0, f.units + 0x6000)
                    f.put(f.victim_type + 0x1be, 32767)
                    f.put(u + 0x114, byte, 'B'); f.put(f.victim_type + 0x17e, modifier & 0xffffffff)
                    f.call(0x51a140, (0, u, damage, kind, 0))
                    hp = f.get(u + 0x10c, 'h')
                    if kind == 12:   # a heal: never scaled, added up to maxdamage
                        assert hp == min(30000 + (damage & 0xffff), 32767), (damage, byte, modifier, hp)
                        rows.append('%d %d %d %d' % (damage, kind, byte & 2, modifier))
                        expected.append('%d' % damage)
                        continue
                    rows.append('%d %d %d %d' % (damage, kind, byte & 2, modifier))
                    expected.append('%d' % ((30000 - hp) & 0xffff))
    output = subprocess.run([binary, '--damage-modifier'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    bad = ['%s port %s native %s' % (row, got, want) for row, got, want in zip(rows, actual, expected)
           if (int(got) & 0xffff) != (int(want) & 0xffff)]
    assert not bad, '\n'.join(bad[:10])
    return 'damage modifier: %d rows match; only an activated unit (+0x114 & 2) scales a hit under 30000' % len(rows)


def bounty():
    """512ae0's kill branch (512c6b..512c99) natively: on a credited kill the
    killer's player (+0xfc) gains the victim type's mogriumbounty (UnitDef
    +0x222, a float) in its resource object's pool ([+0x2510] +0, float,
    uncapped) and lifetime total (+0x18, double), whatever the scenario's
    score flag (game +0x3070 & 4). World: creditKillBounty, kill_credit_test."""
    f = Fixture()
    stubs = {'4c6750': 2, '4c6800': 2, '523510': 1, '50aa20': 1, '51b4f0': 5, '5066a0': 1,
             '512ee0': 5, '5ba5d0': 0, '4ee560': 1, '4dc7f0': 0, '4eba00': 0, '4c7190': 0, '52ac00': 2}
    for name, nargs in stubs.items():
        f.record(name, nargs)
    scenario = f.flags; vtable = f.flags + 0x100; stub = f.flags + 0x200
    f.put(0x62da3c, scenario); f.put(scenario, vtable); f.put(vtable + 0x18, stub)
    f.p.uc.mem_write(stub, b'\xc2\x04\x00')
    f.put(f.game + 0x175dc, f.map)
    resources = f.flags + 0x400
    paid = 0
    for kind, finished, same, hitter, owned, amount in itertools.product((1, 3, 5, 6, 8), (0, 1), (0, 1), (3, 10),
                                                                        (0, 4), (0.0, 25.5)):
        u = f.unit(1, flags=ALIVE | DYING | DEATH_RUNS, player=2)
        f.put(u + 0x116, kind << 4, 'B'); f.put(u + 0x108, 0.0 if finished else 0.5, 'f')
        player = 2 if same else hitter
        f.put(u + 0xfc, player, 'B'); f.put(u + 0x115, 0, 'B')
        f.put(f.victim_type + 0x222, amount, 'f'); f.put(f.game + 0x3070, owned)
        if player != 10:
            f.put(f.game + player * 0x110 + 0x2510, resources)
        f.p.uc.mem_write(resources, bytes(0x40)); f.put(resources, 10.0, 'f'); f.put(resources + 0x18, 100.0, 'd')
        f.call(0x512ae0, (u,))
        credited = kind in (1, 3, 8) and finished and not same and hitter != 10
        want = amount if credited else 0.0
        assert f.get(resources, 'f') - 10.0 == want and f.get(resources + 0x18, 'd') - 100.0 == want, \
            (kind, finished, same, hitter, owned, amount)
        paid += credited and amount != 0
    return 'bounty: a credited kill pays mogriumbounty to the killer player\'s pool and total (%d of 160 cases)' % paid


def losses():
    """512ae0's losses word natively (owner record +0xa2, the end screen's
    'Losses', 500535): which death kinds (+0x116 >> 4) add 1, finished or
    not, whoever hit it (World: retailDeathCountsAsLoss). The local player's alliance byte for the
    owner (game +0x24d1 in its record) is left 0, as it is for an enemy."""
    f = Fixture()
    stubs = {'4c6750': 2, '4c6800': 2, '523510': 1, '50aa20': 1, '51b4f0': 5, '5066a0': 1,
             '512ee0': 5, '5ba5d0': 0, '4ee560': 1, '4dc7f0': 0, '4eba00': 0, '4c7190': 0, '52ac00': 2}
    for name, nargs in stubs.items():
        f.record(name, nargs)
    scenario = f.flags; vtable = f.flags + 0x100; stub = f.flags + 0x200
    f.put(0x62da3c, scenario); f.put(scenario, vtable); f.put(vtable + 0x18, stub)
    f.p.uc.mem_write(stub, b'\xc2\x04\x00')
    f.put(f.game + 0x175dc, f.map)
    f.put(f.game + 3 * 0x110 + 0x2510, f.flags + 0x400)   # the hitter player's resources (the bounty)
    counted = set()
    for kind, finished, hitter in itertools.product(range(16), (0, 1), (3, 10)):
        u = f.unit(1, flags=ALIVE | DYING | DEATH_RUNS, player=2)
        f.put(u + 0x116, kind << 4, 'B'); f.put(u + 0x108, 0.0 if finished else 0.5, 'f')
        f.put(u + 0xfc, hitter, 'B'); f.put(u + 0x115, 0, 'B')
        f.put(f.local + 0xa2, 0, 'H')
        f.call(0x512ae0, (u,))
        if f.get(f.local + 0xa2, 'H'):
            counted.add(kind)
        else:
            assert kind not in counted, (kind, finished, hitter)
    want = {1, 2, 3, 5, 7, 8, 13, 14, 15}
    assert counted == want, sorted(counted)
    return 'losses: 512ae0 counts kinds %s, finished or not, whoever hit it' % sorted(counted)


def unconjure():
    """429af0's unconjure step (negative work, GetBuilt's 429d70(site, 1)
    = -0.5) natively: while the remaining work stays below 1 the site's HP
    word follows it; the step that takes it to 1 or more zeroes the word and
    hits the site with 51a140(site, site, 30000, 11, 0): its own hitter,
    kind 11."""
    f = Fixture()
    f.record('51a140', 5)
    site_type = f.victim_type
    f.put(site_type + 0x216, 0.01, 'f'); f.put(site_type + 0x20e, 100.0, 'f'); f.put(site_type + 0x1be, 100)
    f.put(f.local + 0x10c, f.flags + 0x400)   # the owner's resource object (the refund)
    deaths = 0
    for remaining in (0.25, 0.9, 0.994, 0.995, 0.999):
        u = f.unit(1, player=2); f.put(u + 0x108, remaining, 'f'); f.put(u + 0x10c, 7, 'H')
        f.calls.clear()
        f.call(0x429af0, (u, u, struct.unpack('<i', struct.pack('<f', -0.5))[0]))
        died = remaining + 0.005 >= 1.0
        if died:
            assert f.calls == [('51a140', u, u, 30000, 11, 0)] and f.get(u + 0x10c, 'H') == 0, (remaining, f.calls)
            deaths += 1
        else:
            assert not f.calls, (remaining, f.calls)
    return 'unconjure: the step that leaves no work hits the site itself, 51a140(site, site, 30000, 11, 0) (%d of 5)' % deaths


def crt_health():
    """The CRT action cases natively, entered with their own frame: the
    arguments each hands 51a140, for every type/region combination."""
    f = Fixture()
    f.record('51a140', 5)
    stub, action, context, regions = f.flags - 0x80, f.flags + 0x200, f.flags + 0x400, f.flags + 0x800
    f.put(context, 2); f.put(context + 0xc, regions)
    f.put(regions + 3 * 0x110 + 0x100, 4); f.put(regions + 3 * 0x110 + 0x104, 6)
    f.put(regions + 3 * 0x110 + 0x108, 9); f.put(regions + 3 * 0x110 + 0x10c, 11)
    first = f.units
    f.put(f.game + 2 * 0x110 + 0x2478, first); f.put(f.game + 2 * 0x110 + 0x247c, first + 3 * 0x138)
    cases = 0
    for entry, kind in ((0x4cb106, 12), (0x4cb1b8, 13)):
        stub += 0x80   # a stub of its own: unicorn keeps translated code
        # push ebp; mov ebp,esp; push ebx; push esi; push edi; mov ebx,ctx; mov edi,action; jmp entry
        code = bytes.fromhex('55 89 e5 53 56 57') + b'\xbb' + struct.pack('<I', context) + \
            b'\xbf' + struct.pack('<I', action)
        code += b'\xe9' + struct.pack('<i', entry - (stub + len(code) + 5))
        f.p.uc.mem_write(stub, code)
        for typ, region, amount in itertools.product((0, 7, 8), (3, -1), (20, 65556, -5)):
            us = []
            for i, (t, x, z) in enumerate(((7, 4, 6), (8, 9, 11), (7, 10, 6), (7, 5, 12))):
                u = f.unit(i); f.put(u, t, 'H'); f.put(u + 0x74, x, 'h'); f.put(u + 0x76, z, 'h'); us.append(u)
            f.put(action + 0x4d, typ, 'H'); f.put(action + 0x4f, region & 0xffffffff); f.put(action + 4, amount & 0xffffffff)
            f.calls.clear()
            f.call(stub, (0, 0))
            want = [('51a140', 0, u, amount & 0xffffffff, kind, 0) for u, (t, x, z) in
                    zip(us, ((7, 4, 6), (8, 9, 11), (7, 10, 6), (7, 5, 12)))
                    if region != -1 and (typ in (0, t)) and 4 <= x <= 9 and 6 <= z <= 11]
            assert f.calls == want, (hex(entry), typ, region, amount, f.calls, want)
            cases += 1
    return 'CRT heal/damage: %d cases, 51a140(0, unit, amount, 12 / 13, 0) for the region\'s units' % cases


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_script_test'
    for line in (hitter_gate(), attack_multiplier(), death_weapon_shot(), retirement(), mission_kill(),
                 water_hit(), water_death(), deadly_water(binary), carried_water(binary),
                 self_destruct(), self_destruct_hit(), self_destruct_death(), commander(), cargo(),
                 hp_sample(binary), severity(binary), death_sample(), veteran_reload(),
                 damage_modifier(binary), bounty(), losses(), crt_health(), unconjure()):
        print(line)


if __name__ == '__main__':
    main()
