#!/usr/bin/env python3
"""Differential check of Retail group pacing against the original routines.

Loads randomized group records (through retail's own setters) and units into
the emulated KINGDOMS.icd and compares, case by case, with the engine's
helpers in src/sim/retailgroup.h and retailGroundFormation:

  tick       51b890 group tick incl. straggler pass and the changed-flag
             notification (game+0x3070 bit 0, navigator vt+0x30/vt+0x38)
  world      the World's squad -> record mapping (retailGroupSlot): a Ctrl
             group +N is a plain record N (+8 clear), a formation -N record
             N with +8 set, a computer player's AI squad its own record with
             the AI's flag; 51b890 then 51d1e0 / 51c700 per grouped unit
  query      51c700 centre, 51ce40 radius, 51d1e0 out-of-slot
  check      402b00 head: Move_Ground's two group checks (+50c480 on cancel)
  formation  402880 Move_Ground_Formation, all four stages, response modes 0, 1 and
             the FormationStandby's -1
  vtolcheck  417c20's head (417e02..418052): VTOL_Move's two group checks,
             with flyer altitudes (the same helper as 402b00)
  vtolpatrol 419db0's head (419f9b..41a200): VTOL_Patrol's two group checks,
             the same helper as 402b00 / 417e02, on patrol legs (0x1000412,
             anchored 0x4000 or not)
  vtolform   417750 VTOL_Move_Formation (retailFlightFormation), and as
             VTOL_Move_FormationStandby (0x4000000): back in the slot while
             the group moves, the scan's attack or the loiter point
             (41797e..417ad1)
  standby    the standbys' group check: VTOL_Standby 417527 (51d1e0(unit, 1,
             2), VTOL_Move_FormationStandby at level -1, 0), Standby 407779
             (the same; Move_Ground_FormationStandby with +0x52 = -1, 0) and
             VTOL_LandIfCan 416eda (51d1e0(unit, 1, 3), VTOL_Move_
             FormationStandby at level -2, 5: the landing retires), against
             retailStandbyGroupCheck, each run from its entry past its exits
  suppress   404d70's head (404e12..404f1b): SUPPRESS's group check
             (retailAttackGroupCheck: 51d1e0(unit, 1, 4), rand(3), 50c480 by
             the next mission's flags, the Move_Ground_Formation push at
             level -3 with no target scan, rand(15)), stopped at its stage
             jump table
  patrol     403600's head (40361c..40388d): the ground Patrol's two group
             checks, against retailGroundGroupCheck as 402b00's
  follow     407f90's head (40800c..40810e): Follow_Ground's group check
             (retailGuardGroupCheck: 51d1e0(unit, 1, 4), rand(4) -> 50c480
             and 4d6a50(unit, 0), the Move_Ground_Formation push at level -3
             with response mode 0, rand(10)), stopped at its stage switch
  carried    the tick's member filter (51ba44..51ba8f) with passengers:
             carrier +0xa8 set, every +0x114 bit, BeCarried's flags 0x24;
             it reads none of them, so a passenger is a member at its +0x68
  becarried  the attach 51b4f0 -> 51b5a0, which for a live owner (+0xea 1
             or 2) and a carrier that is no airbase calls 4d49f0: the queue
             cleared (4d6a50(unit, 0)) and BECARRIED put in front (4d7750);
             BeCarried's row (5eb69f: handler 4024a0, flags 0x24) and the
             handler itself, which never runs a group check (51d1e0)

Observation only: nothing from the binary is copied into the engine.

    python3 -B tools/re/check_retail_group.py [--cases N] [--build DIR]
"""
import argparse, os, random, struct, subprocess, sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SETTERS = {  # class -> (count all, count moving, area all, area moving, centre all, centre moving)
    0: (0x50b840, 0x50b8c0, 0x50b940, 0x50b9c0, 0x50ba40, 0x50bb00),
    1: (0x50bc60, 0x50bce0, 0x50bd60, 0x50bde0, 0x50be60, 0x50bf20),
    2: (0x50c080, 0x50c100, 0x50c180, 0x50c200, 0x50c280, 0x50c340),
}
GAME, PLAYER, TABLE, UNITS, TYPES, MOVERS, NAVS, MISSIONS, VT = (
    HEAP, HEAP + 0x2404, HEAP + 0x20000, HEAP + 0x30000, HEAP + 0x40000,
    HEAP + 0x50000, HEAP + 0x60000, HEAP + 0x80000, HEAP + 0x90000)
HOOK30, HOOK38 = HEAP + 0x91000, HEAP + 0x91010
STRIDE = 0x138


def s16(v): return ((v & 0xffff) ^ 0x8000) - 0x8000


class Harness:
    def __init__(self):
        self.p = Icd(); self.uc = self.p.uc; self.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        self.events = []
        def nav30(uc, sp):
            nav = uc.reg_read(UC_X86_REG_ECX); arg = struct.unpack('<i', uc.mem_read(sp, 4))[0]
            self.events.append(('fb', (nav - NAVS) // 0x40, arg)); return 1, 0
        def nav38(uc, sp):
            self.events.append(('notify', (uc.reg_read(UC_X86_REG_ECX) - NAVS) // 0x40)); return 0, 0
        self.p.hooks[HOOK30] = nav30; self.p.hooks[HOOK38] = nav38
        self.draws = []
        def rand(uc, sp):
            n = struct.unpack('<i', uc.mem_read(sp, 4))[0]; self.events.append(('rand', n))
            return 1, self.draws.pop(0) if self.draws else 0
        self.p.hooks[0x535cc0] = rand
        def record(name, nargs, ret=0):
            def f(uc, sp):
                args = struct.unpack('<%di' % nargs, uc.mem_read(sp, 4 * nargs)) if nargs else ()
                self.events.append((name, uc.reg_read(UC_X86_REG_ECX)) + tuple(args)); return nargs, ret
            return f
        self.p.hooks[0x4d6a50] = record('cancel', 2)
        self.p.hooks[0x4d7750] = record('push', 2)
        self.p.hooks[0x4d4d40] = record('stop', 1)
        self.p.hooks[0x4d6a10] = record('sleep', 1)
        def goal(uc, sp):
            vec, radius = struct.unpack('<Ii', uc.mem_read(sp, 8))
            x, y, z = struct.unpack('<iii', uc.mem_read(vec, 12))
            self.events.append(('move', s16(x >> 16), s16(z >> 16), radius)); return 2, 0
        self.p.hooks[0x4d4da0] = goal
        # The target scan and the attack it may put ahead (VTOL_Move_
        # FormationStandby's 4179ef), answered by self.answers.
        self.answers = dict(scan=0, attack=0)
        def scan(uc, sp):
            self.events.append(('scan',)); return 1, self.answers['scan']
        self.p.hooks[0x4d8370] = scan
        def attack(uc, sp):
            args = struct.unpack('<6I', uc.mem_read(sp, 24))
            self.events.append(('attack',) + args); return 6, self.answers['attack']
        self.p.hooks[0x4d7de0] = attack
        self.alloc = HEAP + 0xa0000
        def alloc(uc, sp): return 0, self.alloc
        self.p.hooks[0x4eb9e0] = alloc
        def name(uc, sp):
            text = struct.unpack('<I', uc.mem_read(sp, 4))[0]
            self.events.append(('name', bytes(uc.mem_read(text, 40)).split(b'\0')[0].decode()))
            return 1, uc.reg_read(UC_X86_REG_ECX)
        self.p.hooks[0x4d4bf0] = name
        def ctor(uc, sp):
            args = struct.unpack('<12i', uc.mem_read(sp, 48))
            self.events.append(('formation', args[4], args[5]))
            this = uc.reg_read(UC_X86_REG_ECX); uc.mem_write(this + 4, b'\1'); return 12, this
        self.p.hooks[0x4d6c40] = ctor
        self.uc.hook_add(1 << 2, self._stop, begin=0x402d53, end=0x402d53)  # UC_HOOK_CODE
        # VTOL_Move continues at 418052 once its group checks pass.
        self.uc.hook_add(1 << 2, self._stop, begin=0x418052, end=0x418052)
        # The standbys go on to their stages past their group check:
        # VTOL_Standby 4175c8, Standby 40781c, VTOL_LandIfCan 416f7e.
        for onward in (0x4175c8, 0x40781c, 0x416f7e):
            self.uc.hook_add(1 << 2, self._stop, begin=onward, end=onward)
        # VTOL_Patrol goes on at 41a200 (the flag's rand(10), as 418052)
        # once its group checks pass.
        self.uc.hook_add(1 << 2, self._stop, begin=0x41a200, end=0x41a200)
        self.p.hooks[0x416c50] = record('begin', 4)
        self.p.hooks[0x519b10] = lambda uc, sp: (1, 0)
        def controller(uc, sp):
            this = uc.reg_read(UC_X86_REG_ECX); vec = struct.unpack('<I', uc.mem_read(sp + 4, 4))[0]
            x, y, z = struct.unpack('<iii', uc.mem_read(vec, 12))
            self.events.append(('controller', s16(x >> 16), s16(z >> 16), x, y, z)); return 2, this
        self.p.hooks[0x4e40e0] = controller
        def radius(uc, sp):
            self.events.append(('radius', struct.unpack('<i', uc.mem_read(sp, 4))[0])); return 1, 0
        self.p.hooks[0x4e4540] = radius
        self.p.freeze_hooks()
        self.put('I', 0x62d55c, GAME); self.put('I', PLAYER, 1); self.put('I', PLAYER + 0x84, TABLE)
        self.put('I', VT + 0x30, HOOK30); self.put('I', VT + 0x38, HOOK38)

    def _stop(self, uc, address, size, _):
        self.events.append(('moveground',)); uc.emu_stop()

    def put(self, fmt, address, *values): self.uc.mem_write(address, struct.pack('<' + fmt, *values))
    def get(self, fmt, address): return struct.unpack('<' + fmt, self.uc.mem_read(address, struct.calcsize(fmt)))

    def call(self, address, args, ecx=None, allow_early_stop=None):
        _, error = self.p.call(address, args, ecx=ecx, allow_early_stop=allow_early_stop)
        if error and 'UC_ERR_OK' not in error: raise AssertionError(error)

    def load_record(self, group, rec):
        base = TABLE + group * 0xc4
        self.uc.mem_write(base, b'\0' * 0xc4)
        self.put('I', base + 8, rec['formation'])
        for cls in range(3):
            names = SETTERS[cls]
            for part, (cnt, area, centre) in enumerate((rec['all'][cls], rec['moving'][cls])):
                self.call(names[0 + part], (PLAYER, group, cnt))
                self.call(names[2 + part], (PLAYER, group, area))
                self.call(names[4 + part], (PLAYER, group) + tuple((c << 16) - (1 << 32) * ((c << 16) >= 1 << 31) for c in centre))
        self.call(0x50b7a0, (PLAYER, group, rec['active']))
        self.call(0x50bbc0, (PLAYER, group, rec['gs'])); self.call(0x50bfe0, (PLAYER, group, rec['bs']))
        self.call(0x50c400, (PLAYER, group, rec['changed']))

    def read_record(self, group):
        base = TABLE + group * 0xc4
        out = [self.get('i', base + 0x2c)[0], self.get('i', base + 0xb0)[0], self.get('i', base + 0x58)[0],
               self.get('i', base + 0x84)[0]]
        for off in (0x30, 0x5c, 0x88):
            cnt_a, cnt_m, area_a, area_m = self.get('4i', base + off)
            ca = self.get('3i', base + off + 0x10); cm = self.get('3i', base + off + 0x1c)
            out += [cnt_a, area_a] + [s16(v >> 16) for v in ca] + [cnt_m, area_m] + [s16(v >> 16) for v in cm]
        return out

    def load_unit(self, i, u):
        a = UNITS + i * STRIDE; t = TYPES + i * 0x300; m = MOVERS + i * 0x40; n = NAVS + i * 0x40
        ms = MISSIONS + i * 0x80
        self.uc.mem_write(a, b'\0' * STRIDE); self.uc.mem_write(t, b'\0' * 0x300); self.uc.mem_write(ms, b'\0' * 0x80)
        self.put('I', a + 8, m if u['mover'] else 0); self.put('I', m, n); self.put('I', n, VT)
        self.uc.mem_write(m + 0x36, struct.pack('<H', u['terrain']))
        self.put('I', a + 0x60, ms if u['mission'] else 0); self.put('I', ms + 0x5a, u['flags'])
        for k, v in zip((0x68, 0x6c, 0x70), u['fixed']): self.put('i', a + k, v)
        self.uc.mem_write(a + 0x78, struct.pack('<hh', u['fx'], u['fz']))
        self.put('f', a + 0x108, 0.0 if u['complete'] else 0.5)
        self.put('I', a + 0x130, u['ustate']); self.put('I', a + 0xb4, t); self.put('I', a + 0xb8, PLAYER)
        self.put('I', a + 0xc8, u['group'])
        if u.get('carrier'):   # aboard: the carrier pointer and every +0x114 bit
            self.put('I', a + 0xa8, u['carrier']); self.uc.mem_write(a + 0x114, b'\xff')
        self.put('i', t + 0x162, u['type_max']); self.put('i', t + 0x16e, u['water']); self.put('i', t + 0x172, u['road'])
        self.uc.mem_write(t + 0x194, struct.pack('<h', u['depth'])); self.put('I', t + 0x260, u['tflags'])
        return a


def scaled(u):
    if u['terrain'] & 0x800: return (u['road'] * u['type_max']) >> 16
    if u['terrain'] & 0x1000: return (u['water'] * u['type_max']) >> 16
    return u['type_max']


def kind(u):
    if u['tflags'] & 0x800: return 2
    if u['mover'] and u['tflags'] & 0x80000 and u['depth'] > 0: return 1
    return 0


def member_text(u):
    return '%d %d %d %d %d %d' % (kind(u), int(u['mover']), s16(u['fixed'][0] >> 16), s16(u['fixed'][1] >> 16),
                                  s16(u['fixed'][2] >> 16), u['fx'] * u['fz'])


def record_text(r):
    parts = [r['formation'] != 0, r['active'], r['changed'], r['gs'], r['bs']]
    for cls in range(3):
        for cnt, area, c in (r['all'][cls], r['moving'][cls]): parts += [cnt, area] + list(c)
    return ' '.join(str(int(v)) for v in parts)


def random_record(rng, formation=True, spread=600):
    def part():
        n = rng.choice([0, 1, 2, 3, 5, 9]); return (n, rng.randrange(0, 40) if n else rng.choice([0, 3]),
                                                 tuple(rng.randrange(0, spread) for _ in range(3)))
    return dict(formation=int(formation), active=rng.random() < 0.85, changed=rng.random() < 0.3,
                gs=rng.choice([0, rng.randrange(1, 300000)]), bs=rng.choice([0, rng.randrange(1, 300000)]),
                all=[part() for _ in range(3)], moving=[part() for _ in range(3)])


def random_unit(rng, group, spread=600):
    tflags = rng.choice([0, 0, 0, 0x80000, 0x800])
    flags = rng.choice([0, 0x3000400, 0x3000400, 0x11000000, 0x5012000, 0x15000400, 0x1000000, 0x2000000])
    return dict(group=group, mover=rng.random() < 0.95, mission=flags != 0 or rng.random() < 0.2, flags=flags,
                terrain=rng.choice([0, 0, 0x800, 0x1000]), tflags=tflags, depth=rng.choice([0, 4]),
                fixed=tuple((rng.randrange(0, spread) << 16) | rng.randrange(65536) for _ in range(3)),
                fx=rng.randrange(1, 5), fz=rng.randrange(1, 5), complete=rng.random() < 0.95,
                ustate=0x1000000 | (0x1000 if rng.random() < 0.05 else 0) if rng.random() < 0.95 else 0,
                type_max=rng.randrange(20000, 200000), road=rng.choice([65536, 78643]),
                water=rng.choice([32768, 65536]))


def eligible(u):
    return bool(u['ustate'] & 0x1000000) and not u['ustate'] & 0x1000 and u['complete'] and u['mover']


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--cases', type=int, default=400)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build-dbg', 'tmp'))
    a = ap.parse_args()
    os.makedirs(a.build, exist_ok=True)
    driver = os.path.join(a.build, 'retail_group_driver')
    subprocess.check_call(['g++', '-std=c++20', '-O1', '-I', os.path.join(ROOT, 'src'), '-o', driver,
                           os.path.join(ROOT, 'tools', 're', 'retail_group_driver.cpp')])
    ours = subprocess.Popen([driver], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def ask(text, lines=1):
        ours.stdin.write(text + '\n'); ours.stdin.flush()
        return [ours.stdout.readline().strip() for _ in range(lines)]
    h = Harness(); rng = random.Random(0x51b890); counts = dict(tick=0, query=0, check=0, formation=0)
    # For the suppress mode; added before any code runs, as Unicorn keeps the
    # blocks it has translated (50c480 runs natively in the check mode too).
    h.uc.hook_add(1 << 2, lambda uc, address, size, _: (h.events.append(('stages',)), uc.emu_stop()),
                  begin=0x404f1b, end=0x404f1b)
    h.uc.hook_add(1 << 2, lambda uc, address, size, _: h.events.append(('leave',)), begin=0x50c480, end=0x50c480)
    h.uc.hook_add(1 << 2, lambda uc, address, size, _: (h.events.append(('stages',)), uc.emu_stop()),
                  begin=0x40810e, end=0x40810e)
    # Patrol (403600) goes on at 40388d (the flag's rand(10)) once its group
    # checks pass, as Move_Ground at 402d53.
    h.uc.hook_add(1 << 2, h._stop, begin=0x40388d, end=0x40388d)
    stats = dict(notified=0, slowed=0, reform=0, wait=0, cancel=0, carried=0)
    crng = random.Random(0x51ba44)
    # 51b890 tick
    for case in range(a.cases):
        h.events.clear()
        notify = rng.random() < 0.5; kindp = rng.choice([1, 2, 3])
        h.put('I', GAME + 0x3070, int(notify)); h.uc.mem_write(PLAYER + 0xea, bytes([kindp, 0]))
        groups = rng.sample(range(1, 100), 4)
        recs = {g: random_record(rng, formation=rng.random() < 0.8) for g in range(1, 100)}
        for g in range(1, 100):
            if g not in groups: recs[g] = dict(recs[g], formation=0)
            h.load_record(g, recs[g])
        n = rng.randrange(1, 14)
        units = [random_unit(rng, rng.choice(groups + [0])) for _ in range(n)]
        # Passengers, from their own stream so the other cases stay as they were.
        for u in units:
            if crng.random() < 0.3:
                u['carrier'] = UNITS + 60 * STRIDE; u['flags'] = 0x24; u['mission'] = True
                stats['carried'] += eligible(u) and u['group'] in groups
        # cluster a group so the straggler test fires
        for u in units[: n // 2]: u['fixed'] = tuple(((300 + rng.randrange(40)) << 16) for _ in range(3))
        for i, u in enumerate(units): h.load_unit(i, u)
        h.put('I', PLAYER + 0x74, UNITS); h.put('I', PLAYER + 0x78, UNITS + (n - 1) * STRIDE)
        h.events.clear(); h.call(0x51b890, ())
        theirs = [h.read_record(g) for g in range(1, 100)]
        fb = {e[1]: e[2] for e in h.events if e[0] == 'fb'}
        notified = sorted(e[1] for e in h.events if e[0] == 'notify')
        text = 'tick %d\n' % int(notify and kindp in (1, 2)) + '\n'.join(record_text(recs[g]) for g in range(1, 100))
        text += '\n%d\n' % n + '\n'.join('%d %d %s %d %d' % (u['group'], int(eligible(u)), member_text(u),
                                          u['flags'] if u['mission'] else 0, scaled(u)) for u in units)
        out = ask(text, 101)
        mine = [[int(v) for v in line.split()] for line in out[:99]]
        paced = [int(v) for v in out[99].split()]
        mine_notified = sorted(int(v) for v in out[100].split())
        for g in range(99):
            a_ = theirs[g]; b_ = mine[g]
            # retail returns record fields as stored; ours mirrors them
            if a_ != b_:
                raise AssertionError(('tick', case, g + 1, notify, kindp, notified, mine_notified, a_, b_))
        want_paced = [fb.get(i, 0) for i in range(n)]
        if want_paced != paced: raise AssertionError(('paced', case, want_paced, paced))
        if notified != mine_notified: raise AssertionError(('notify', case, notified, mine_notified))
        counts['tick'] += 1; stats['notified'] += len(notified)
        stats['slowed'] += sum(1 for g in groups if theirs[g - 1][2] not in (0, recs[g]['gs']))
    # The World's mapping from engine squads to records: a Ctrl group +N is
    # CreateSquad N (record N, +8 clear: never paced), a formation -N is
    # CreateSquad N f (record N, +8 set). Retail is loaded as those commands
    # leave it (48e170 -> 50b700 / 522170 -> 50b4f0); the driver derives the
    # record and +8 from the squads itself (retailGroupSlot). Records start
    # inactive, as created; then 51b890 and each grouped unit's queries.
    # Its own random stream, so the other modes' cases stay as they were.
    wrng = random.Random(0x50b700)
    counts['world'] = 0; stats['plain'] = 0
    for case in range(a.cases):
        # A computer player's case (owner kind 3): its AI's squads 1..99
        # (Unit::aiGroup), each flagged by set-active 50b700 or not
        # (World::aiFormation).
        computer = wrng.random() < 0.4
        h.put('I', GAME + 0x3070, 0); h.uc.mem_write(PLAYER + 0xea, bytes([3 if computer else 1, 0]))
        numbers = wrng.sample(range(1, 100) if computer else range(1, 11), 3)
        kinds = {g: wrng.choice([1, -1]) for g in numbers}          # +N plain, -N formation
        n = wrng.randrange(2, 14)
        units = []
        for _ in range(n):
            g = wrng.choice(numbers + [0])
            u = random_unit(wrng, g)
            u['squad'] = g * kinds[g] if g and not computer else 0
            u['ai'] = g if computer else 0; u['aiFormation'] = int(bool(g) and computer and kinds[g] < 0)
            if wrng.random() < 0.7: u['flags'] = wrng.choice([0x3000400, 0x3000400, 0x15000400]); u['mission'] = True
            units.append(u)
        for u in units[: n // 2]: u['fixed'] = tuple(((300 + wrng.randrange(40)) << 16) for _ in range(3))
        recs = {}
        for g in range(1, 100):
            r = random_record(wrng); r['active'] = False; r['changed'] = False
            r['formation'] = int(any(u['squad'] == -g or (u['ai'] == g and u['aiFormation']) for u in units))
            recs[g] = r; h.load_record(g, r)
        for i, u in enumerate(units): h.load_unit(i, u)
        h.put('I', PLAYER + 0x74, UNITS); h.put('I', PLAYER + 0x78, UNITS + (n - 1) * STRIDE)
        h.events.clear(); h.call(0x51b890, ())
        theirs = [h.read_record(g) for g in range(1, 100)]
        fb = {e[1]: e[2] for e in h.events if e[0] == 'fb'}
        want_units = []
        for i, u in enumerate(units):
            if not u['group']: want_units.append('-'); continue
            addr = UNITS + i * STRIDE
            s3 = h.p.call(0x51d1e0, (addr, 1, 3))[0]; s4 = h.p.call(0x51d1e0, (addr, 0, 4))[0]
            h.call(0x51c700, (HEAP + 0xb0000, addr, 0, 1))
            cx, _, cz = (s16(v >> 16) for v in h.get('3i', HEAP + 0xb0000))
            want_units.append('%d %d %d %d' % (s3, s4, cx, cz))
            if u['squad'] > 0 or (u['ai'] and not u['aiFormation']):
                # Retail itself: a plain group gets no check and no pacing bit.
                if s3 or s4 or fb.get(i, 0): raise AssertionError(('retail paced a plain group', case, i))
                stats['plain'] += 1
        text = 'world\n' + '\n'.join(record_text(recs[g]) for g in range(1, 100))
        text += '\n%d\n' % n + '\n'.join('%d %d %d %d %s %d %d' % (
            u['squad'], u['ai'], u['aiFormation'], int(eligible(u)), member_text(u),
            u['flags'] if u['mission'] else 0, scaled(u)) for u in units)
        out = ask(text, 100 + n)
        mine = [[int(v) for v in line.split()] for line in out[:99]]
        for g in range(99):
            if theirs[g] != mine[g]:
                raise AssertionError(('world record', case, g + 1, kinds.get(g + 1), theirs[g], mine[g]))
        paced = [int(v) for v in out[99].split()]
        if [fb.get(i, 0) for i in range(n)] != paced:
            raise AssertionError(('world paced', case, [u['squad'] for u in units],
                                  [fb.get(i, 0) for i in range(n)], paced))
        if out[100:] != want_units:
            raise AssertionError(('world queries', case, [u['squad'] for u in units], want_units, out[100:]))
        counts['world'] += 1
    # 51c700 / 51ce40 / 51d1e0
    for case in range(a.cases * 10):
        rec = random_record(rng, spread=500); rec['active'] = rng.random() < 0.9
        g = rng.randrange(1, 99); h.load_record(g, rec)
        u = random_unit(rng, g, spread=500); addr = h.load_unit(0, u)
        aa, bb, level = rng.randrange(2), rng.randrange(2), rng.randrange(0, 7)
        h.call(0x51c700, (HEAP + 0xb0000, addr, aa, bb))
        cx, cy, cz = (s16(v >> 16) for v in h.get('3i', HEAP + 0xb0000))
        radius = h.p.call(0x51ce40, (addr, aa, bb))[0]
        slot = h.p.call(0x51d1e0, (addr, bb, level))[0]
        allf = int(bool(bb and u['mission'] and u['flags'] & 0x4000000))
        mine = ask('query %s %s %d %d %d %d' % (record_text(rec), member_text(u), aa, bb, level, allf))[0].split()
        want = [cx, cy, cz, radius, slot]
        if [int(v) for v in mine] != want:
            raise AssertionError(('query', case, want, mine, rec, u, aa, bb, level))
        counts['query'] += 1
    # 402b00 head
    for case in range(a.cases * 5):
        rec = random_record(rng, spread=400); rec['active'] = True; g = rng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['flags'] = 0x3000400 | (0x8000000 if rng.random() < 0.5 else 0); u['ustate'] = 0x1000000
        u['fixed'] = (u['fixed'][0], 0, u['fixed'][2]); addr = h.load_unit(0, u)
        ms = MISSIONS; gx, gz = rng.randrange(0, 400) << 16, rng.randrange(0, 400) << 16
        for k, v in zip((0x22, 0x26, 0x2a), (gx, 0, gz)): h.put('i', ms + k, v)
        h.put('i', ms + 0x52, 0); draw = rng.randrange(4); h.draws = [draw]; h.events.clear()
        # Deliberately stopped at the Move_Ground hand-off (0x402d53, _stop).
        h.call(0x402b00, (addr, ms, 0), allow_early_stop=True)
        flags_after = h.get('I', ms + 0x5a)[0]
        kinds = [e[0] for e in h.events]
        if 'cancel' in kinds: want = 1
        elif 'formation' in kinds: want = [e for e in h.events if e[0] == 'formation'][0][2]
        else: want = 0
        draws = kinds.count('rand')
        rec_after = h.read_record(g)
        out = ask('check %s %s %d %d 0 %d %d 0 %d %d' % (record_text(rec), member_text(u), u['flags'],
                                                       u['fixed'][0], u['fixed'][2], gx, gz, draw), 2)
        r, f, d = (int(v) for v in out[0].split()); rmine = [int(v) for v in out[1].split()]
        if (r, f, d) != (want, flags_after, draws) or (want == 1 and rmine != rec_after):
            raise AssertionError(('check', case, (want, hex(flags_after), draws, rec_after), (r, hex(f), d, rmine)))
        key = {-2: 'reform', 4: 'wait', 1: 'cancel'}.get(want)
        if key: stats[key] += 1
        counts['check'] += 1
    # 402880 Move_Ground_Formation
    for case in range(a.cases * 5):
        rec = random_record(rng, spread=400); rec['active'] = rng.random() < 0.95; g = rng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['flags'] = rng.choice([0x11000000, 0x15000400]); addr = h.load_unit(0, u)
        ms = MISSIONS; stage = rng.randrange(0, 4); level = rng.choice([-2, -1, 2, 3, 4, 5])
        mode = rng.choice([0, 0, 1, -1]); draw = rng.randrange(5)
        h.uc.mem_write(ms + 5, bytes([stage])); h.put('i', ms + 0x56, level); h.put('i', ms + 0x52, mode)
        h.put('I', addr + 0x130, 0x1000000 | rng.choice([0, 0x10000, 0x20000]))
        h.put('I', addr + 0xa8, 0); h.draws = [draw]; h.events.clear()
        result = h.p.call(0x402880, (addr, ms, 0), ecx=None)[0]
        stage_after = h.get('B', ms + 5)[0]
        calls = []
        for e in h.events:
            if e[0] in ('stop', 'move', 'rand'): calls.append(e[0])
            elif e[0] == 'sleep': calls.append('sleep%d' % e[2])
        gx = s16(h.get('i', ms + 0x22)[0] >> 16); gz = s16(h.get('i', ms + 0x2a)[0] >> 16)
        out = ask('formation %s %s %d %d 0 %d %d %d' % (record_text(rec), member_text(u), stage, level, mode,
                                                        u['flags'], draw))[0].split()
        r, st = int(out[0]), int(out[1])
        mine_calls = [c for c in out[4].split(',') if c and c not in ('-', 'centre', 'scan')]
        ok = (r, st) == (result, stage_after) and mine_calls == calls
        if 'centre' in out[4]: ok = ok and (int(out[2]), int(out[3])) == (gx, gz)
        if not ok:
            raise AssertionError(('formation', case, dict(stage=stage, level=level, mode=mode),
                                  (result, stage_after, gx, gz, calls), out))
        counts['formation'] += 1
    # 417c20 head: VTOL_Move's group checks, compared with retailGroundGroupCheck
    # on full 3-D positions (flyer altitude in y).
    counts['vtolcheck'] = 0; counts['vtolform'] = 0
    for case in range(a.cases * 5):
        rec = random_record(rng, spread=400); rec['active'] = True; g = rng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['tflags'] = 0x800 if rng.random() < 0.8 else u['tflags']
        u['flags'] = 0x1000402 | (0x8000000 if rng.random() < 0.5 else 0); u['ustate'] = 0x1000000
        addr = h.load_unit(0, u)
        h.put('I', addr + 0xa4, 1); h.uc.mem_write(addr + 0x12a, b'\0')
        ms = MISSIONS; gx, gy, gz = (rng.randrange(0, 400) << 16 for _ in range(3))
        for k, v in zip((0x22, 0x26, 0x2a), (gx, gy, gz)): h.put('i', ms + k, v)
        h.uc.mem_write(ms + 5, bytes([rng.randrange(0, 4)]))
        draw = rng.randrange(4); h.draws = [draw]; h.events.clear()
        h.call(0x417c20, (addr, ms, 0), allow_early_stop=True)
        if not h.events or h.events[-1][0] not in ('moveground', 'cancel') and 'formation' not in [e[0] for e in h.events]:
            raise AssertionError(('vtolcheck-exit', case, h.events))
        flags_after = h.get('I', ms + 0x5a)[0]
        kinds = [e[0] for e in h.events]
        if 'cancel' in kinds: want = 1
        elif 'formation' in kinds: want = [e for e in h.events if e[0] == 'formation'][0][2]
        else: want = 0
        draws = kinds.count('rand'); rec_after = h.read_record(g)
        out = ask('check %s %s %d %d %d %d %d %d %d %d' % (record_text(rec), member_text(u), u['flags'],
                  u['fixed'][0], u['fixed'][1], u['fixed'][2], gx, gy, gz, draw), 2)
        r, f, d = (int(v) for v in out[0].split()); rmine = [int(v) for v in out[1].split()]
        if (r, f, d) != (want, flags_after, draws) or (want == 1 and rmine != rec_after):
            raise AssertionError(('vtolcheck', case, (want, hex(flags_after), draws, rec_after), (r, hex(f), d, rmine)))
        key = {-2: 'vreform', 4: 'vwait', 1: 'vcancel'}.get(want)
        if key: stats[key] = stats.get(key, 0) + 1
        counts['vtolcheck'] += 1
    # 419db0 head: VTOL_Patrol's group checks, the same helper again.
    counts['vtolpatrol'] = 0
    for case in range(a.cases * 5):
        rec = random_record(rng, spread=400); rec['active'] = True; g = rng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['tflags'] = 0x800 if rng.random() < 0.8 else u['tflags']
        u['flags'] = 0x1000412 | rng.choice([0, 0x4000]) | (0x8000000 if rng.random() < 0.5 else 0)
        u['ustate'] = 0x1000000
        addr = h.load_unit(0, u)
        h.put('I', addr + 0xa4, 1); h.uc.mem_write(addr + 0x12a, b'\0')
        ms = MISSIONS; gx, gy, gz = (rng.randrange(0, 400) << 16 for _ in range(3))
        for k, v in zip((0x22, 0x26, 0x2a), (gx, gy, gz)): h.put('i', ms + k, v)
        h.uc.mem_write(ms + 5, bytes([rng.randrange(0, 3)]))
        draw = rng.randrange(4); h.draws = [draw]; h.events.clear()
        h.call(0x419db0, (addr, ms, 0), allow_early_stop=True)
        kinds = [e[0] for e in h.events]
        if not h.events or kinds[-1] not in ('moveground', 'cancel') and 'formation' not in kinds:
            raise AssertionError(('vtolpatrol-exit', case, h.events))
        flags_after = h.get('I', ms + 0x5a)[0]
        if 'cancel' in kinds: want = 1
        elif 'formation' in kinds:
            want = [e for e in h.events if e[0] == 'formation'][0][2]
            if 'push' not in kinds: raise AssertionError(('vtolpatrol push', case, h.events))
        else: want = 0
        draws = kinds.count('rand'); rec_after = h.read_record(g)
        out = ask('check %s %s %d %d %d %d %d %d %d %d' % (record_text(rec), member_text(u), u['flags'],
                  u['fixed'][0], u['fixed'][1], u['fixed'][2], gx, gy, gz, draw), 2)
        r, f, d = (int(v) for v in out[0].split()); rmine = [int(v) for v in out[1].split()]
        if (r, f, d) != (want, flags_after, draws) or (want == 1 and rmine != rec_after):
            raise AssertionError(('vtolpatrol', case, (want, hex(flags_after), draws, rec_after), (r, hex(f), d, rmine)))
        key = {-2: 'preform', 4: 'pwait', 1: 'pcancel'}.get(want)
        if key: stats[key] = stats.get(key, 0) + 1
        counts['vtolpatrol'] += 1
    # 417750 VTOL_Move_Formation, and VTOL_Move_FormationStandby
    for case in range(a.cases * 5):
        rec = random_record(rng, spread=400); rec['active'] = rng.random() < 0.95; g = rng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['tflags'] = 0x800 if rng.random() < 0.85 else u['tflags']
        u['flags'] = rng.choice([0x1000000, 0x1000402, 0x1000002, 0x5000002, 0x5000002, 0x5000402]); addr = h.load_unit(0, u)
        h.put('I', addr + 0xa4, 1); h.uc.mem_write(addr + 0x12a, b'\0')
        heading = rng.randrange(65536); h.uc.mem_write(addr + 0x7e, struct.pack('<H', heading))
        ms = MISSIONS; stage = rng.randrange(0, 3); level = rng.choice([-2, -2, -1, 4, 4, 0, -1, 3])
        draws = [rng.randrange(10), rng.randrange(10), rng.randrange(0x1000)]
        if rng.random() < 0.5: draws[0] = rng.randrange(20)
        found = rng.choice([0, 0, UNITS + STRIDE]); h.answers.update(scan=found, attack=rng.choice([0, 1]))
        h.uc.mem_write(ms + 5, bytes([stage])); h.put('i', ms + 0x56, level)
        for k in (0x22, 0x26, 0x2a): h.put('i', ms + k, rng.randrange(0, 400) << 16)
        h.draws = list(draws); h.events.clear()
        result = h.p.call(0x417750, (addr, ms, 0), ecx=None)[0]
        stage_after = h.get('B', ms + 5)[0]
        calls = []
        for e in h.events:
            if e[0] in ('begin', 'rand', 'scan'): calls.append(e[0])
            elif e[0] == 'radius': calls.append('move' if e[1] == 16 else 'loiter' if e[1] == 64 else 'radius%d' % e[1])
            elif e[0] == 'sleep': calls.append('sleep%d' % e[2])
        attacks = [e for e in h.events if e[0] == 'attack']
        if attacks and attacks != [('attack', addr, found, 0, 0, 1, 0)]:
            raise AssertionError(('vtolform attack', case, attacks))
        if attacks and not found: raise AssertionError(('vtolform attack without a find', case))
        gx = s16(h.get('i', ms + 0x22)[0] >> 16); gz = s16(h.get('i', ms + 0x2a)[0] >> 16)
        loiter = [e for e in h.events if e[0] == 'controller'] if 'loiter' in calls else []
        point = loiter[-1][3:] if loiter else (0, 0, 0)
        point = tuple(v - (1 << 32) if v >= 1 << 31 else v for v in point)
        wait = h.get('I', ms + 6)[0]
        out = ask('vtolformation %s %s %d %d %d %d %d %d %d %d %d %d %d %d' % (
            record_text(rec), member_text(u), stage, level, int(bool(u['tflags'] & 0x800)), u['flags'],
            draws[0], draws[1], draws[2], heading, int(bool(found) and h.answers['attack'] != 0),
            *(v - (1 << 32) if v >= 1 << 31 else v for v in u['fixed'])))[0].split()
        r, st = int(out[0]), int(out[1])
        mine_calls = [c for c in out[4].split(',') if c and c not in ('-', 'centre')]
        mine_point = tuple(int(v) for v in out[5:8]); mine_wait = int(out[8]) & ~1
        ok = (r, st) == (result, stage_after) and mine_calls == calls and mine_point == point and mine_wait == wait
        if 'centre' in out[4]: ok = ok and (int(out[2]), int(out[3])) == (gx, gz)
        if not ok:
            raise AssertionError(('vtolform', case, dict(stage=stage, level=level, flags=hex(u['flags'])),
                                  (result, stage_after, gx, gz, calls, point, hex(wait)), out))
        key = ('vfs%d' if u['flags'] & 0x4000000 else 'vf%d') % result
        if 'loiter' in calls: key += 'loiter'
        stats[key] = stats.get(key, 0) + 1
        counts['vtolform'] += 1
    # The standbys' group check (retailStandbyGroupCheck).
    counts['standby'] = 0
    sites = [('VTOL_Standby', 0x417350, 0x5010000, 'VTOL_Move_FormationStandby'),
             ('Standby', 0x407770, 0x5012000, 'Move_Ground_FormationStandby'),
             ('VTOL_LandIfCan', 0x416cd0, 0x5000400, 'VTOL_Move_FormationStandby')]
    for case in range(a.cases * 6):
        index = case % 3; site, handler, row, name = sites[index]
        rec = random_record(rng, spread=rng.choice([100, 400])); rec['active'] = rng.random() < 0.95
        g = rng.randrange(1, 99); h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = True; u['mission'] = True; u['ustate'] = 0x1000000
        u['tflags'] = 0x800 if index != 1 else rng.choice([0, 0, 0x80000])
        u['flags'] = row | rng.choice([0, 0x8000000]); addr = h.load_unit(0, u)
        h.put('I', addr + 0xa4, 1); h.uc.mem_write(addr + 0x12a, b'\0')
        ms = MISSIONS; h.uc.mem_write(ms + 5, bytes([rng.randrange(0, 3)]))
        h.draws = []; h.events.clear()
        ret, err = h.p.call(handler, (addr, ms, 0), allow_early_stop=True)
        if err and 'UC_ERR_OK' not in err: raise AssertionError(err)
        kinds = [e[0] for e in h.events]
        formations = [e for e in h.events if e[0] == 'formation']
        names = [e[1] for e in h.events if e[0] == 'name']
        if formations:
            if names != [name] or 'push' not in kinds: raise AssertionError((site, case, h.events))
            want = (ret, formations[0][1], formations[0][2])
        elif kinds and kinds[-1] == 'moveground': want = (-1, 0, 0)
        else: raise AssertionError((site, case, 'no formation and no stages', h.events))
        if 'rand' in kinds: raise AssertionError((site, case, 'a draw', h.events))
        out = ask('standbyhead %s %s %d %d' % (record_text(rec), member_text(u), index, u['flags']))[0].split()
        if tuple(int(v) for v in out) != want:
            raise AssertionError(('standby', site, case, want, out))
        key = '%s%s' % (site, 'out' if formations else 'in')
        stats[key] = stats.get(key, 0) + 1
        counts['standby'] += 1
    # 404d70's head: SUPPRESS's group check, then its stages (stopped at 404f1b).
    counts['suppress'] = 0
    following = MISSIONS + 0x100
    for case in range(a.cases * 10):
        rec = random_record(rng, spread=rng.choice([100, 400, 1500])); rec['active'] = rng.random() < 0.95
        g = rng.randrange(1, 99); h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = rng.random() < 0.97; u['mission'] = True
        u['flags'] = 0x1000410 | (0x8000000 if rng.random() < 0.5 else 0); u['ustate'] = 0x1000000
        u['tflags'] = rng.choice([0, 0, 0, 0x80000]); u['fixed'] = (u['fixed'][0], 0, u['fixed'][2])
        addr = h.load_unit(0, u); ms = MISSIONS
        nxt = rng.choice([None, 0x3000400, 0x5012000, 0x0, 0x1000410, 0x15000400, 0x11000000])
        h.uc.mem_write(following, b'\0' * 0x80)
        if nxt is None: h.put('I', ms + 0x66, 0)
        else: h.put('I', ms + 0x66, following); h.put('I', following + 0x5a, nxt)
        h.uc.mem_write(ms + 5, bytes([rng.randrange(5)]))
        ev = rng.choice([0, 0x10000, 0x100000, 0x40, 0x700])
        draw = rng.randrange(15); h.draws = [draw]; h.events.clear()
        ret, err = h.p.call(0x404d70, (addr, ms, ev), allow_early_stop=True)
        if err and 'UC_ERR_OK' not in err: raise AssertionError(err)
        calls = ''
        for e in h.events:
            if e[0] == 'rand': calls += 'rand%d,' % e[1]
            elif e[0] == 'leave': calls += 'leave,'
            elif e[0] == 'formation':
                if e[1:] != (0, -3): raise AssertionError(('suppress formation args', case, e))
                calls += 'formation,'
        kinds = [e[0] for e in h.events]
        if 'formation' in kinds and 'push' not in kinds: raise AssertionError(('suppress push', case, h.events))
        want = (-1 if 'stages' in kinds else ret, h.get('I', ms + 0x5a)[0], calls or '-')
        rec_after = h.read_record(g)
        out = ask('suppresshead %s %s %d %d %d' % (record_text(rec), member_text(u), u['flags'], nxt or 0, draw), 2)
        r, f, c = out[0].split(); rmine = [int(v) for v in out[1].split()]
        if (int(r), int(f), c) != want or rmine != rec_after:
            raise AssertionError(('suppress', case, want, (r, f, c), nxt, rec_after, rmine))
        key = {-1: 'sin', 8: 'sleave', 0: 'sform'}.get(want[0], 's%d' % want[0])
        stats[key] = stats.get(key, 0) + 1
        counts['suppress'] += 1
    # 407f90's head: Follow_Ground's group check, then its stages (stopped at 40810e).
    counts['follow'] = 0
    for case in range(a.cases * 10):
        rec = random_record(rng, spread=rng.choice([100, 400, 1500])); rec['active'] = rng.random() < 0.95
        g = rng.randrange(1, 99); h.load_record(g, rec)
        u = random_unit(rng, g, spread=400); u['mover'] = rng.random() < 0.97; u['mission'] = True
        u['flags'] = 0x3000200 | (0x8000000 if rng.random() < 0.5 else 0); u['ustate'] = 0x1000000
        u['tflags'] = rng.choice([0, 0, 0, 0x80000]); u['fixed'] = (u['fixed'][0], 0, u['fixed'][2])
        addr = h.load_unit(0, u); ms = MISSIONS
        ward = h.load_unit(1, dict(random_unit(rng, 0), ustate=0x1000000, tflags=0))
        h.put('I', ms + 0x16, ward); h.put('I', addr + 0xa8, 0)
        h.uc.mem_write(ms + 5, bytes([rng.randrange(3)]))
        draw = rng.randrange(4); h.draws = [draw]; h.events.clear()
        ret, err = h.p.call(0x407f90, (addr, ms, rng.choice([0, 0x10, 0x700])), allow_early_stop=True)
        if err and 'UC_ERR_OK' not in err: raise AssertionError(err)
        calls = ''
        for e in h.events:
            if e[0] == 'rand': calls += 'rand%d,' % e[1]
            elif e[0] == 'leave': calls += 'leave,'
            elif e[0] == 'cancel':
                if e[2:] != (addr, 0): raise AssertionError(('follow cancel args', case, e))
                calls += 'cancel,'
            elif e[0] == 'formation':
                if e[1:] != (0, -3): raise AssertionError(('follow formation args', case, e))
                calls += 'formation,'
        kinds = [e[0] for e in h.events]
        if 'formation' in kinds and 'push' not in kinds: raise AssertionError(('follow push', case, h.events))
        want = (-1 if 'stages' in kinds else ret, h.get('I', ms + 0x5a)[0], calls or '-')
        rec_after = h.read_record(g)
        out = ask('followhead %s %s %d %d' % (record_text(rec), member_text(u), u['flags'], draw), 2)
        r, f, c = out[0].split(); rmine = [int(v) for v in out[1].split()]
        if (int(r), int(f), c) != want or rmine != rec_after:
            raise AssertionError(('follow', case, want, (r, f, c), rec_after, rmine))
        key = {-1: 'fin', 2: 'fcancel', 0: 'fform'}.get(want[0], 'f%d' % want[0])
        stats[key] = stats.get(key, 0) + 1
        counts['follow'] += 1
    # 403600's head: the ground Patrol's two group checks, against the same
    # helper as 402b00 (World's Patrol host calls retailGroundGroupCheck).
    # Its own random stream, so the other modes' cases stay as they were.
    prng = random.Random(0x403600); counts['patrol'] = 0
    for case in range(a.cases * 5):
        rec = random_record(prng, spread=400); rec['active'] = True; g = prng.randrange(1, 99)
        h.load_record(g, rec)
        u = random_unit(prng, g, spread=400); u['mover'] = True; u['mission'] = True
        u['flags'] = 0x3000410 | prng.choice([0, 0x4000]) | (0x8000000 if prng.random() < 0.5 else 0)
        u['ustate'] = 0x1000000
        u['fixed'] = (u['fixed'][0], 0, u['fixed'][2]); addr = h.load_unit(0, u)
        ms = MISSIONS; gx, gz = prng.randrange(0, 400) << 16, prng.randrange(0, 400) << 16
        for k, v in zip((0x22, 0x26, 0x2a), (gx, 0, gz)): h.put('i', ms + k, v)
        h.put('i', ms + 0x52, 0); h.uc.mem_write(ms + 5, bytes([prng.randrange(0, 4)]))
        draw = prng.randrange(4); h.draws = [draw]; h.events.clear()
        h.call(0x403600, (addr, ms, 0), allow_early_stop=True)
        flags_after = h.get('I', ms + 0x5a)[0]
        kinds = [e[0] for e in h.events]
        if 'cancel' in kinds: want = 1
        elif 'formation' in kinds:
            want = [e for e in h.events if e[0] == 'formation'][0][2]
            if 'push' not in kinds: raise AssertionError(('patrol push', case, h.events))
        elif 'moveground' in kinds: want = 0
        else: raise AssertionError(('patrol-exit', case, h.events))
        draws = kinds.count('rand'); rec_after = h.read_record(g)
        out = ask('check %s %s %d %d 0 %d %d 0 %d %d' % (record_text(rec), member_text(u), u['flags'],
                                                       u['fixed'][0], u['fixed'][2], gx, gz, draw), 2)
        r, f, d = (int(v) for v in out[0].split()); rmine = [int(v) for v in out[1].split()]
        if (r, f, d) != (want, flags_after, draws) or (want == 1 and rmine != rec_after):
            raise AssertionError(('patrol', case, (want, hex(flags_after), draws, rec_after), (r, hex(f), d, rmine),
                                  dict(unit=(u['fixed'][0] >> 16, u['fixed'][2] >> 16), goal=(gx >> 16, gz >> 16))))
        key = {-2: 'greform', 4: 'gwait', 1: 'gcancel'}.get(want)
        if key: stats[key] = stats.get(key, 0) + 1
        counts['patrol'] += 1
    ours.stdin.close(); ours.wait()
    counts['becarried'] = becarried()
    print('PASS: %s; %s' % (', '.join('%s %d' % kv for kv in counts.items()),
                            ', '.join('%s %d' % kv for kv in stats.items())))


def becarried():
    """The attach and BeCarried, in an emulator of their own (the attach's
    ground, sound, packet and selection calls are observation boundaries)."""
    p = Icd(); uc = p.uc; events = []
    def record(name, nargs, ret=0):
        def f(uc_, sp):
            args = struct.unpack('<%di' % nargs, uc_.mem_read(sp, 4 * nargs)) if nargs else ()
            events.append((name, uc_.reg_read(UC_X86_REG_ECX)) + tuple(args)); return nargs, ret
        return f
    for addr, nargs in ((0x5065f0, 1), (0x506630, 1), (0x421e10, 2), (0x50a9c0, 3), (0x5207d0, 1)):
        p.hooks[addr] = record('boundary', nargs)
    p.hooks[0x4e6730] = lambda uc_, sp: (2, struct.unpack('<I', uc_.mem_read(sp, 4))[0])
    p.hooks[0x4e8030] = record('send', 1)
    p.hooks[0x4d6a50] = record('cancel', 2)
    p.hooks[0x4d7750] = record('push', 2)
    p.hooks[0x4d6a10] = record('sleep', 1)
    p.hooks[0x519b70] = record('weapons', 1)
    p.hooks[0x51d1e0] = record('groupcheck', 3)
    alloc = HEAP + 0xa0000
    p.hooks[0x4eb9e0] = lambda uc_, sp: (0, alloc)
    def name(uc_, sp):
        text = struct.unpack('<I', uc_.mem_read(sp, 4))[0]
        events.append(('name', bytes(uc_.mem_read(text, 16)).split(b'\0')[0].decode()))
        return 1, uc_.reg_read(UC_X86_REG_ECX)
    p.hooks[0x4d4bf0] = name
    def ctor(uc_, sp):
        this = uc_.reg_read(UC_X86_REG_ECX); uc_.mem_write(this + 4, b'\1'); return 12, this
    p.hooks[0x4d6c40] = ctor
    p.freeze_hooks()
    def put(fmt, addr, *v): uc.mem_write(addr, struct.pack('<' + fmt, *v))
    def get(fmt, addr): return struct.unpack('<' + fmt, uc.mem_read(addr, struct.calcsize(fmt)))[0]
    # BeCarried's row: handler, flags (+0x11) and name (S, read off .rdata).
    row = 0x5eb69f
    assert get('I', row + 4) == 0x4024a0 and get('I', row + 0x11) == 0x24
    assert bytes(uc.mem_read(get('I', row + 0x15), 10)) == b'BeCarried\0'
    put('I', 0x62d55c, GAME)
    cases = 0
    rng = random.Random(0x4d49f0)
    for case in range(64):
        uc.mem_write(UNITS, bytes(4 * STRIDE)); uc.mem_write(TYPES, bytes(0x600)); uc.mem_write(MOVERS, bytes(0x80))
        put('I', GAME + 0x14e84, UNITS); put('I', GAME + 0x14e88, UNITS + 3 * STRIDE)
        passenger, carrier = UNITS + STRIDE, UNITS + 2 * STRIDE
        kind = rng.choice((1, 2, 3)); airbase = rng.random() < 0.25
        uc.mem_write(PLAYER + 0xea, bytes([kind])); put('I', PLAYER, 1)
        for i, u in ((1, passenger), (2, carrier)):
            put('H', u + 2, i); put('I', u + 0x130, 0x1000000); put('I', u + 0xb8, PLAYER)
            put('I', u + 0xb4, TYPES + i * 0x300); put('I', u + 8, MOVERS + i * 0x40)
        put('I', TYPES + 2 * 0x300 + 0x260, 0x200 if airbase else 0)
        events.clear()
        _, err = p.call(0x51b4f0, (passenger, carrier, -1, 0, 1))
        assert not err, err
        assert get('I', passenger + 0xa8) == carrier and get('I', carrier + 0xac) == passenger
        kinds = [e[0] for e in events]
        want = kind in (1, 2) and not airbase
        if want:
            cancel = [e for e in events if e[0] == 'cancel']
            assert cancel and cancel[0][2:] == (passenger, 0), events
            assert ('name', 'BECARRIED') in events and 'push' in kinds, events
            assert kinds.index('cancel') < kinds.index('push')
        else:
            assert 'push' not in kinds and 'cancel' not in kinds, events
        # The handler while carried (stages 0 and 1), and once set down.
        mission = HEAP + 0xb0000
        for carried, stage in ((True, 0), (True, 1), (False, 0), (False, 1)):
            uc.mem_write(mission, bytes(0x80)); uc.mem_write(mission + 5, bytes([stage]))
            put('I', passenger + 0xa8, carrier if carried else 0)
            events.clear()
            r, err = p.call(0x4024a0, (passenger, mission, 0))
            assert not err, err
            kinds = [e[0] for e in events]
            assert 'groupcheck' not in kinds
            if not carried: assert r == 5 and not kinds
            elif stage == 0: assert r == 1 and kinds == ['weapons'] and events[0][1:] == (passenger, 3), events
            else: assert r == 2 and kinds == ['sleep'] and events[0][1:] == (mission, 10), events
        cases += 1
    return cases


if __name__ == '__main__': main()
