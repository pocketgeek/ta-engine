#!/usr/bin/env python3
"""Compare retail veterancy with src/sim/retailveterancy.h (and Unit::vetMul).

Three native pieces, each against the port's --veterancy rows:

  * the kill credit of the death routine 512ae0: its death-kind switch
    (512b71, table 512eb8/512ec4) through the kill count, score and credit
    gates to 512d2a, run natively on a synthetic victim: the hitter player's
    kill count (record +0xa0) and score (+0xa4, off when game +0x3070 & 4),
    and the killer's experience. Every death kind 0..15, a
    finished or unfinished victim (+0x108), hitter player (+0xfc, 10 = none)
    against owner (+0xfd), a killer at +0xf8 or none, the victim type's
    experiencepoints (+0x1c2) and the killer's experience (+0xf0) near the
    32-bit wrap. The killer's own type is given noveteran and no speed: the
    credit reads nothing of it.
  * getLevel 519310 and getMult 5193c0: experience over the unit's own
    experiencepoints, noveteran (+0x268 & 0x10), `v F` (+0xf4).
  * setLevel 519370: an authored level's experience through _ftol.
  * the uncredited deaths: deadly water (51a140, kind 13), the mission Kill
    (513130, kind 10), and the mission Capture's owner change (514da0).

    python3 tools/re/check_veterancy.py [build-o2/retail_script_test]
"""
import itertools
import random
import struct
import subprocess
import sys

from emu import Icd, HEAP, RETURN_MAGIC
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_ESI, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP

STACK_TOP = 0x70000000 + 0x100000 - 0x2000


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/retail_script_test'
    p = Icd()
    game, victim, victim_type, killer, killer_type, owner, totals, stub, out = (
        HEAP + i * 0x20000 for i in range(9))

    def put(address, value, fmt='I'):
        p.uc.mem_write(address, struct.pack('<' + fmt, value))

    def get(address, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    put(0x62d55c, game)
    for player in range(11):   # the per-player totals 512c8b adds the victim's value into
        put(game + 0x2510 + player * 0x110, totals + player * 0x20)

    def credit(kind, finished, hitter, owner_player, has_killer, points, xp, score_off, count):
        p.uc.mem_write(victim, bytes(0x138)); p.uc.mem_write(killer, bytes(0x138))
        for player in range(11):   # the hitter's kill count (+0xa0, word) and score (+0xa4)
            put(game + 0x24a4 + player * 0x110, 0); put(game + 0x24a8 + player * 0x110, 0)
        put(game + 0x3070, 4 if score_off else 0, 'B')
        put(victim + 0xb4, victim_type); put(victim + 0xb8, owner); put(owner + 0xeb, owner_player, 'B')
        put(victim + 0x116, kind << 4, 'B'); put(victim + 0x108, 0.0 if finished else 0.25, 'f')
        put(victim + 0xfc, hitter, 'B'); put(victim + 0xfd, owner_player, 'B')
        put(victim + 0xf8, killer if has_killer else 0)
        put(victim_type + 0x1c2, points & 0xffffffff)
        put(killer + 0xb4, killer_type); put(killer + 0xf0, xp & 0xffffffff); put(killer + 0xfe, count, 'H')
        put(killer_type + 0x268, 0x10, 'B'); put(killer_type + 0x1c2, 0)
        for reg, value in ((UC_X86_REG_ESI, victim), (UC_X86_REG_EBX, 0),
                           (UC_X86_REG_ESP, STACK_TOP), (UC_X86_REG_EBP, STACK_TOP + 0x100)):
            p.uc.reg_write(reg, value)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(0x512b71, 0x512d2a, count=400)
        got = get(killer + 0xf0, 'i')
        kills = get(game + 0x24a4 + hitter * 0x110, 'H'); score = get(game + 0x24a8 + hitter * 0x110, 'i')
        own = get(killer + 0xfe, 'H')   # 512ccd: the killer's own kill count, a word
        return '%d %d %d %d' % (got if has_killer else xp, kills, score, own if has_killer else count)

    def call_stub(code):
        p.uc.mem_write(stub, code + b'\xc3')
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(stub)
        assert not error, error

    def rel(at, target):
        return struct.pack('<i', target - (at + 5))

    def unit_with(xp, points, noveteran, multiplier):
        p.uc.mem_write(victim, bytes(0x138)); p.uc.mem_write(victim_type, bytes(0x300))
        put(victim + 0xb4, victim_type); put(victim_type + 0x268, 0x10 if noveteran else 0, 'B')
        put(victim_type + 0x1c2, points & 0xffffffff); put(victim + 0xf0, xp & 0xffffffff)
        put(victim + 0xf4, multiplier, 'f')

    def level(xp, points, noveteran, multiplier):
        unit_with(xp, points, noveteran, multiplier)
        level_value, error = p.call(0x519310, (victim,))
        assert not error, error
        # push unit; call 5193c0; fstp dword [out]
        code = b'\x68' + struct.pack('<I', victim) + b'\xe8' + rel(stub + 5, 0x5193c0) \
            + b'\xd9\x1d' + struct.pack('<I', out)
        call_stub(code)
        return struct.unpack('<i', struct.pack('<I', level_value))[0], get(out)

    def set_level(xp, level_value, points, noveteran, multiplier):
        unit_with(xp, points, noveteran, multiplier)
        _, error = p.call(0x519370, (victim, level_value))
        assert not error, error
        return get(victim + 0xf0, 'i')

    rng = random.Random(0x512ce0)
    shipped = (1, 2, 3, 5, 7, 15, 18, 27, 30, 50, 100, 300, 666, 1000)
    rows, expected = [], []
    for kind, finished, (hitter, owner_player), has_killer in itertools.product(
            range(16), (0, 1), ((0, 1), (1, 0), (2, 2), (10, 3), (0, 0), (7, 9)), (0, 1)):
        points = rng.choice(shipped); xp = rng.choice((0, 5, 666, 0x7fffffff - 3, -40))
        score_off = rng.choice((0, 0, 1)); count = rng.choice((0, 1, 41, 0xffff))
        rows.append('c %d %d %d %d %d %d %d %d %d' % (kind, finished, -1 if hitter == 10 else hitter, owner_player,
                                                    has_killer, points, xp, score_off, count))
        expected.append(credit(kind, finished, hitter, owner_player, has_killer, points, xp, score_off, count))
    credits = len(rows)
    for _ in range(400):
        points = rng.choice(shipped + (0, -5))
        xp = rng.choice((0, 1, points - 1, points, 3 * points + 1, 9 * points, 10 * points, 11 * points,
                         rng.randrange(0, 20000), -rng.randrange(1, 2000), 0x7fffffff, -0x80000000))
        noveteran = rng.choice((0, 0, 0, 1)); multiplier = rng.choice((0.0, 0.0, 0.0, 1.5, 2.5))
        if points == 0 and not noveteran and multiplier == 0.0:
            points = 1   # retail's idiv faults on a zero divisor
        if points == -1 and xp == -0x80000000:
            xp = 0
        lv, mult = level(xp, points, noveteran, multiplier)
        rows.append('l %d %d %d %r' % (xp, points, noveteran, multiplier))
        expected.append('%d %d' % (lv, mult))
    levels = len(rows) - credits
    for _ in range(300):
        points = rng.choice(shipped)
        lv = rng.choice(range(-2, 13)); xp = rng.choice((0, 77, -3))
        noveteran = rng.choice((0, 0, 0, 1)); multiplier = rng.choice((0.0, 0.0, 0.0, 2.0))
        rows.append('s %d %d %d %d %r' % (xp, lv, points, noveteran, multiplier))
        expected.append('%d' % set_level(xp, lv, points, noveteran, multiplier))
    output = subprocess.run([binary, '--veterancy'], input='\n'.join(rows) + '\n',
                            text=True, capture_output=True, check=True)
    actual = output.stdout.splitlines()
    if len(actual) != len(expected):
        raise AssertionError('row count %d, expected %d' % (len(actual), len(expected)))
    for row, got, want in zip(rows, actual, expected):
        if got != want:
            raise AssertionError('%s\n  port   %s\n  native %s' % (row, got, want))
    credited = sum(1 for r, w in zip(rows[:credits], expected[:credits]) if w.split()[0] != r.split()[-3])
    kills = sum(1 for w in expected[:credits] if w.split()[1] != '0')
    scored = sum(1 for w in expected[:credits] if w.split()[2] != '0')
    print('veterancy: %d kill-credit cases (%d credited, %d kills counted, %d scored), %d getLevel/getMult, '
          '%d setLevel rows match' % (credits, credited, kills, scored, levels, len(rows) - credits - levels))
    print(uncredited_deaths())


def uncredited_deaths():
    """The death kinds of two deaths no attack causes, natively.

    * Deadly water (51e13f) hurts through 51a140 with no attacker and damage
      kind 13: the lethal hit keeps the last hitter (+0xf8, +0xfc), records
      kind 13 at +0x10f (what 51e36c hands 512610) and sets the dying bit.
    * The mission Kill (0x4d36eb -> 513130) hands 512610 death kind 10 for
      every unit this machine runs (owner +0xea 1 or 2), none of a remote one.
    Neither 13 nor 10 is a kind the credit switch at 512b71 credits."""
    p = Icd()
    game, units, victim_type, owner, remote, hitter = (HEAP + i * 0x20000 for i in range(6))

    def put(address, value, fmt='I'):
        p.uc.mem_write(address, struct.pack('<' + fmt, value))

    def get(address, fmt='I'):
        return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize(fmt)))[0]

    calls = []
    def record(name, nargs):
        def hook(uc, sp):
            calls.append((name,) + struct.unpack('<%dI' % nargs, uc.mem_read(sp, 4 * nargs)))
            return nargs, 0
        return hook
    p.hooks[0x52ae30] = record('52ae30', 2)
    p.hooks[0x512610] = record('512610', 2)
    p.hooks[0x512ae0] = record('512ae0', 1)
    put(0x62d55c, game)
    put(owner, 1); put(owner + 0xea, 2, 'B')
    put(remote, 1); put(remote + 0xea, 3, 'B')
    put(victim_type + 0x1be, 100)

    def unit(index, record_):
        u = units + index * 0x138
        p.uc.mem_write(u, bytes(0x138))
        put(u + 2, index, 'H'); put(u + 0x130, 0x1000000); put(u + 0xb4, victim_type); put(u + 0xb8, record_)
        put(u + 0x10c, 5, 'H')
        return u
    put(game + 0x14e84, units); put(game + 0x14e88, units + 3 * 0x138)
    for i in range(4):
        p.uc.mem_write(units + i * 0x138, bytes(0x138))
    v = unit(1, owner)
    put(v + 0xf8, hitter); put(v + 0xfc, 4, 'B')
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    _, error = p.call(0x51a140, (0, v, 40, 13, 0))
    assert not error, error
    assert get(v + 0xf8) == hitter and get(v + 0xfc, 'B') == 4, 'water damage replaced the last hitter'
    assert get(v + 0x10f, 'B') == 13 and get(v + 0x130) & 0x1000, 'water damage was not a kind-13 kill'
    assert not calls, calls
    unit(1, owner); unit(2, remote)
    _, error = p.call(0x513130)
    assert not error, error
    assert calls == [('52ae30', v, 1), ('512610', v, 10)], calls
    # The mission Capture (0x4d3c57, the verb matched): unit id and player in
    # the argument block [ebp+0x10]; it changes the owner through 514da0.
    calls.clear(); p.hooks[0x514da0] = record('514da0', 3)
    v = unit(1, owner)
    player = 1; record_ = game + 0x2404 + player * 0x110; slot = HEAP + 0x380000
    p.uc.mem_write(record_, bytes(0x110)); put(record_, 1); put(record_ + 0xea, 2, 'B'); put(record_ + 0xeb, 1, 'B')
    put(record_ + 0xe8, 1, 'H'); put(record_ + 0x50, slot); p.uc.mem_write(slot, bytes(0x100))
    block = HEAP + 0x390000; put(block, 1, 'H'); put(block + 4, player, 'B')
    frame = STACK_TOP - 0x400
    put(frame, 0); put(frame + 4, RETURN_MAGIC); put(frame + 0x10, block)
    p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_ESP, frame - 0x2b0)
    p.uc.emu_start(0x4d3c57, RETURN_MAGIC, count=200)
    assert calls == [('514da0', v, record_, 0)], calls
    return ('uncredited deaths: deadly water kills with kind 13 keeping the last hitter; mission Kill sends'
            ' kind 10; mission Capture changes the owner through 514da0')


if __name__ == '__main__':
    main()
