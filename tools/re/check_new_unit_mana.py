#!/usr/bin/env python3
"""Check how retail fills a new unit's mana pool, natively, against the port.

The pool is a {current, maximum} pair of floats at unit +0xd8 / +0xdc, and
four small routines are all that write it: 401000 (current 0, maximum given),
4010d0 (spend if affordable), 401100 (add, capped at the maximum) and 401120
(set, capped at the maximum). This script

  * scans .text for every call to them and to the creation chain (S): 401000
    is called once, from 511990, which only 511f60 calls, which only 512060
    and 512460 call -- every new unit (production, a skirmish start, a mission
    placement, Create, a revive, a change of owner) starts with an empty pool;
    401120 only from the placement record (514cf2) and SetAttribute (4d3ed2);
    401100 only from regeneration (40119e) and the ManaMe cheat (4263c8);
  * runs 401000 (E): the current value becomes 0, the maximum the argument;
  * runs the record reader's percentages (4c8dc3..4c8df0, the getter 543190
    native with the CRT atoi, only its key lookup 543110 answered) (E):
    HealthPercentage defaults to 100 and ManaPercentage to 0, read as atoi
    into words;
  * runs the placement's HP and mana (514c9d..514cf7) and SetAttribute's
    (4d3e6a..4d3e8a, 4d3eb9..4d3ed7) with 401120 native (E), for a grid of
    MaxMana, maxdamage and percentages, and requires the same pool bits and
    hit points from `new_unit_mana_test --native` (MissionScript::percentMana /
    percentHp);
  * runs SetAttribute from its unit lookup (4d3dd0..4d3f59) (E): only a unit
    whose object lives, a dying one too, is written (4d3e27);
  * runs the regeneration step 401160 (E): ManaRechargeRate is added only when
    the construction left (+0x108) is 0, and the sum is capped at MaxMana;
  * reads the tenth argument of every 512060 call (S) and runs the player's
    resource object constructor 401240 (E, an empty pool) and 512060's storage
    credit 5123ce..512417 (E) against `new_unit_mana_test --storage`
    (World::creditCreationStorage): a game opens with its starting units'
    mogriumstorage, 5000 for a Monarch.

    python3 tools/re/check_new_unit_mana.py [build-o2/new_unit_mana_test]
"""
import struct
import subprocess
import sys

import capstone
from emu import Icd, HEAP, ICD, STACK, STACK_SZ
from unicorn.x86_const import (UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESI,
                               UC_X86_REG_ESP, UC_X86_REG_FPCW)

TEXT = (0x401000, 0x1e9912, 0x1000)


def calls(targets):
    """Every direct call in .text to one of `targets`, as {target: [site, ...]}."""
    data = open(ICD, 'rb').read()
    vma, size, off = TEXT
    text = data[off:off + size]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    found = {t: [] for t in targets}
    at = 0
    while at < len(text):
        moved = False
        for i in md.disasm(text[at:at + 0x4000], vma + at):
            moved = True
            if i.mnemonic == 'call' and i.op_str.startswith('0x'):
                t = int(i.op_str, 16)
                if t in found:
                    found[t].append(i.address)
            at = i.address + i.size - vma
        if not moved:
            at += 1
    return found


def static_writers():
    got = calls((0x401000, 0x4010d0, 0x401100, 0x401120, 0x511990, 0x511f60))
    want = {0x401000: [0x511c9d], 0x401120: [0x4d3ed2, 0x514cf2], 0x401100: [0x40119e, 0x4263c8],
            0x4010d0: [0x401201, 0x42811d, 0x5302d6], 0x511990: [0x511fae], 0x511f60: [0x512274, 0x51253f]}
    for t, sites in want.items():
        assert sorted(got[t]) == sites, (hex(t), [hex(s) for s in got[t]])
    return ('the pool is written only by 401000 (511990), 401120 (514cf2 placement, 4d3ed2 SetAttribute), '
            '401100 (40119e regeneration, 4263c8 ManaMe) and 4010d0 (spending); 511990 runs under 511f60 '
            'from 512060 and 512460 only')


# 512060's tenth argument (the first push before each call), S: 1 credits the
# new unit's mogriumstorage to its owner at once (5123ce), 0 does not.
CREDITS = {0x401eeb: 0, 0x4059ca: 0, 0x406f0c: 0, 0x41f49e: 0, 0x420541: 0, 0x426582: 1,
           0x4caf16: 1, 0x4cbc03: 1, 0x4cd307: 1, 0x4d2faf: 1, 0x4d3293: 1, 0x4d36cd: 1,
           0x513431: 0, 0x514c83: 1, 0x514eeb: 0, 0x51e352: 1}


# The stdcalls made between 512060's pushes, with their argument counts (S:
# 5085c0 ret 8, 515ac0 ret 4, 5389c0 ret 8).
NESTED = {0x5085c0: 2, 0x515ac0: 1, 0x5389c0: 2}


def storage_arguments():
    """Every call of 512060 and the value its first push (the tenth argument,
    [ebp+0x2c]) carries."""
    data = open(ICD, 'rb').read()
    vma, size, off = TEXT
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    assert sorted(calls((0x512060,))[0x512060]) == sorted(CREDITS), 'the callers of 512060 changed'
    for site, want in CREDITS.items():
        # 512060 is stdcall with ten dwords: the first of the ten pushes before
        # the call (sub esp, 0xc counts three) is the tenth argument.
        start = site - 0x80
        code = list(md.disasm(data[off + start - vma:off + site - vma + 5], start))
        while code and code[-1].address != site:
            code.pop()
        pushed, first, skip = 0, None, 0
        for i in reversed(code[:-1]):
            if i.mnemonic == 'call':
                # a nested stdcall's own arguments come just before it
                skip = NESTED[int(i.op_str, 16)]
                continue
            if i.mnemonic == 'push' and skip:
                skip -= 1
                continue
            if i.mnemonic == 'push':
                pushed += 1
            elif i.mnemonic == 'sub' and i.op_str == 'esp, 0xc':
                pushed += 3
            if pushed == 10:
                first = i
                break
        assert first is not None and first.mnemonic == 'push' and first.op_str in ('0', '1'), (hex(site), first)
        assert int(first.op_str) == want, (hex(site), first.op_str, want)
    return ('512060 credits storage (tenth argument 1) from the skirmish start 4d3200, the placement record '
            '514b40, Create 4d35c0, the CRT 4cad50/4cd080, 4264a0 and 51de30; production, the builders, 513330 '
            'and the change of owner pass 0')


def empty_resources(p):
    """401240, the player's resource object (4f6910 makes one for each of the
    ten records before the game starts): its pool starts at 0."""
    obj = HEAP + 0x20000
    p.uc.mem_write(obj, b'\xff' * 0x190)
    _, error = p.call(0x401240, (), ecx=obj)
    assert not error, error
    stored, capacity = struct.unpack('<ff', p.uc.mem_read(obj, 8))
    total = struct.unpack('<d', p.uc.mem_read(obj + 0x18, 8))[0]
    assert (stored, capacity, total) == (0.0, 0.0, 0.0), (stored, capacity, total)
    return '401240: a player starts with an empty pool (stored, capacity and lifetime total 0)'


def credit_storage(p):
    """5123ce..512417 natively: (flag, storage, stored, capacity, total) -> the
    resource object after."""
    unit, tp, record, obj, frame = (HEAP + 0x30000 + i * 0x1000 for i in range(5))

    def run(flag, storage, stored, capacity, total):
        for a in (unit, tp, record, obj):
            p.uc.mem_write(a, bytes(0x400))
        p.uc.mem_write(unit + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(unit + 0xb8, struct.pack('<I', record))
        p.uc.mem_write(record + 0x10c, struct.pack('<I', obj))
        p.uc.mem_write(tp + 0x206, struct.pack('<f', storage))
        p.uc.mem_write(obj, struct.pack('<ff', stored, capacity))
        p.uc.mem_write(obj + 0x18, struct.pack('<d', total))
        p.uc.mem_write(frame + 0x2c, struct.pack('<I', flag))
        p.uc.reg_write(UC_X86_REG_ESI, unit)
        p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_ESP, frame - 0x100)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(0x5123ce, 0x512417)
        s, c = struct.unpack('<II', p.uc.mem_read(obj, 8))
        t = struct.unpack('<Q', p.uc.mem_read(obj + 0x18, 8))[0]
        return s, c, t
    return run


def finished_feature(p):
    """51230c..51232c natively (reached only for a finished creation, 5122e4
    tests the sixth argument, S): a type with isfeature (+0x260 & 0x1000000,
    the four walls) leaves the new unit dying (+0x130 |= 0x1000) with death
    kind 9 (+0x10f), which its update's 512610 turns into its feature. The
    completion 429990 does the same (check_construction_completion.py)."""
    unit, tp = HEAP + 0x40000, HEAP + 0x41000
    got = []
    for flags in (0, 0x1000000, 0x40000, 0x1040000):
        p.uc.mem_write(unit, bytes(0x200)); p.uc.mem_write(tp, bytes(0x300))
        p.uc.mem_write(unit + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(unit + 0x130, struct.pack('<I', 0x1000000))
        p.uc.mem_write(tp + 0x260, struct.pack('<I', flags))
        p.uc.reg_write(UC_X86_REG_ESI, unit); p.uc.reg_write(UC_X86_REG_EDI, unit + 0x130)
        p.uc.emu_start(0x51230c, 0x51232c)
        got.append((struct.unpack('<I', p.uc.mem_read(unit + 0x130, 4))[0], p.uc.mem_read(unit + 0x10f, 1)[0]))
    assert got == [(0x1000000, 0), (0x1001000, 9), (0x1000000, 0), (0x1001000, 9)], got
    return '51230c: a finished isfeature creation is left dying with death kind 9'


def cloak_order(p):
    """511c04..511c90 natively: the new unit's cloak order bit (+0x130 & 0x200)
    is UnitDef +0x260 & 0x40, init_cloaked (parsed at 4c0100), whatever the
    unit held before."""
    unit, tp = HEAP + 0x42000, HEAP + 0x43000
    got = []
    for flags, before in ((0, 0), (0x40, 0), (0, 0x200), (0x40, 0x200), (0xffffffbf, 0x200)):
        p.uc.mem_write(unit, bytes(0x200)); p.uc.mem_write(tp, bytes(0x300))
        p.uc.mem_write(unit + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(unit + 0x130, struct.pack('<I', before))
        p.uc.mem_write(tp + 0x260, struct.pack('<I', flags))
        p.uc.reg_write(UC_X86_REG_ESI, unit); p.uc.reg_write(UC_X86_REG_EBX, 0)
        p.uc.emu_start(0x511c04, 0x511c90)
        got.append(bool(struct.unpack('<I', p.uc.mem_read(unit + 0x130, 4))[0] & 0x200))
    assert got == [False, True, False, True, False], got
    return '511c77: a new unit starts with its cloak order on exactly when its type has init_cloaked'


def init_pool(p):
    pool = HEAP + 0x100
    p.uc.mem_write(pool, struct.pack('<ff', 77.0, 5.0))
    _, error = p.call(0x401000, (struct.unpack('<I', struct.pack('<f', 123.5))[0],), ecx=pool)
    assert not error, error
    cur, top = struct.unpack('<ff', p.uc.mem_read(pool, 8))
    assert (cur, top) == (0.0, 123.5), (cur, top)
    return '401000: a new pool holds 0 of its MaxMana'


def record_defaults():
    p = Icd()
    record, text = HEAP + 0x1000, HEAP + 0x2000
    answers = {}

    def lookup(uc, sp):   # 543110(key) -> the value string, or 0 when the key is missing
        key = bytes(uc.mem_read(struct.unpack('<I', uc.mem_read(sp, 4))[0], 32)).split(b'\0')[0].decode()
        value = answers.get(key)
        if value is None:
            return 1, 0
        uc.mem_write(text, value.encode() + b'\0')
        return 1, text
    p.hooks[0x543110] = lookup
    p.freeze_hooks()
    rows = []
    for given in ({}, {'HealthPercentage': '25', 'ManaPercentage': '40'},
                  {'HealthPercentage': '50.9', 'ManaPercentage': '150'}, {'ManaPercentage': 'x'}):
        answers.clear(); answers.update(given)
        p.uc.mem_write(record, bytes(0x40))
        p.uc.reg_write(UC_X86_REG_ESI, record); p.uc.reg_write(UC_X86_REG_EBX, HEAP + 0x3000)
        p.uc.reg_write(UC_X86_REG_ESP, STACK + STACK_SZ - 0x2000)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(0x4c8dc3, 0x4c8df0)
        rows.append(struct.unpack('<hh', p.uc.mem_read(record + 0x1a, 4)))
    assert rows == [(100, 0), (25, 40), (50, 150), (100, 0)], rows
    return 'the record reads HealthPercentage (default 100) and ManaPercentage (default 0) with atoi into words'


def percentages(p):
    """(maxmana bits, maxdamage, pct) -> (placement mana bits, hp), (SetAttribute mana bits, hp)."""
    unit, tp, record, frame = HEAP + 0x4000, HEAP + 0x5000, HEAP + 0x6000, STACK + STACK_SZ - 0x3000

    def run(start, end, maxmana, maxhp, pct, setattr):
        p.uc.mem_write(unit, bytes(0x200)); p.uc.mem_write(tp, bytes(0x300)); p.uc.mem_write(record, bytes(0x40))
        p.uc.mem_write(unit + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(unit + 0xd8, struct.pack('<fI', 9.0, maxmana))
        p.uc.mem_write(tp + 0x1be, struct.pack('<I', maxhp))
        p.uc.mem_write(record + 0x1a, struct.pack('<hh', 100, pct))
        p.uc.mem_write(frame + 0x10, struct.pack('<i', pct))
        p.uc.reg_write(UC_X86_REG_ESI, unit)
        p.uc.reg_write(UC_X86_REG_EDI, pct if setattr else record)
        p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_ESP, frame - 0x100)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(start, end)

    def placed(maxmana, maxhp, pct):
        run(0x514c9d, 0x514cf7, maxmana, maxhp, pct, False)
        mana = struct.unpack('<I', p.uc.mem_read(unit + 0xd8, 4))[0]
        # the placement's HP takes the record's HealthPercentage: run it again with it as pct
        p.uc.mem_write(record + 0x1a, struct.pack('<h', pct))
        p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_ESP, frame - 0x100)
        p.uc.reg_write(UC_X86_REG_ESI, unit); p.uc.reg_write(UC_X86_REG_EDI, record)
        p.uc.emu_start(0x514c9d, 0x514cd2)
        return mana, struct.unpack('<H', p.uc.mem_read(unit + 0x10c, 2))[0]

    def scripted(maxmana, maxhp, pct):
        run(0x4d3eb9, 0x4d3ed7, maxmana, maxhp, pct, True)
        mana = struct.unpack('<I', p.uc.mem_read(unit + 0xd8, 4))[0]
        run(0x4d3e6a, 0x4d3e8a, maxmana, maxhp, pct, True)
        return mana, struct.unpack('<H', p.uc.mem_read(unit + 0x10c, 2))[0]
    return placed, scripted


def regeneration(p):
    unit, tp = HEAP + 0x8000, HEAP + 0x9000
    p.hooks  # 51e4d0 (the cloak bit) is hooked by the caller
    out = []
    for left, cur in ((0.0, 0.0), (0.5, 0.0), (0.0, 999.9), (0.0, 1000.0)):
        p.uc.mem_write(unit, bytes(0x200)); p.uc.mem_write(tp, bytes(0x300))
        p.uc.mem_write(unit + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(unit + 0x130, struct.pack('<I', 0x1000000))
        p.uc.mem_write(unit + 0x108, struct.pack('<f', left))
        p.uc.mem_write(unit + 0xd8, struct.pack('<ff', cur, 1000.0))
        p.uc.mem_write(tp + 0x1a6, struct.pack('<f', 1.0 / 3.0))
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        _, error = p.call(0x401160, (unit,))
        assert not error, error
        out.append(struct.unpack('<f', p.uc.mem_read(unit + 0xd8, 4))[0])
    third = struct.unpack('<f', struct.pack('<f', 1.0 / 3.0))[0]
    assert out == [third, 0.0, 1000.0, 1000.0], out
    return '401160: ManaRechargeRate is added only with no construction left (+0x108 == 0), capped at MaxMana'


def set_attribute_gate():
    """4d3dd0..4d3f59, SetAttribute with two arguments, from the unit lookup
    on (the attribute's name 5389a0 and the CRT stricmp 5d5bb0 answered): it
    writes a unit whose object lives (+0x130 & 0x1000000), a dying one too
    (+0x1000), and leaves a freed one as it was."""
    from unicorn import UC_HOOK_CODE
    p = Icd()
    game, units, tp, args, name = (HEAP + 0x10000 + i * 0x4000 for i in range(5))
    p.uc.mem_write(0x62d55c, struct.pack('<I', game))
    p.uc.mem_write(game + 0x14e84, struct.pack('<II', units, units + 7 * 0x138))
    p.uc.mem_write(name, b'HealthPercentage\0')
    p.hooks[0x5389a0] = lambda uc, sp: (2, name)

    def stricmp(uc, sp):
        a, b = struct.unpack('<2I', uc.mem_read(sp, 8))
        text = [bytes(uc.mem_read(x, 32)).split(b'\0')[0].lower() for x in (a, b)]
        return 0, int(text[0] != text[1])
    p.hooks[0x5d5bb0] = stricmp
    p.freeze_hooks()
    for end in (0x4d3e8a, 0x4d3ed7, 0x4d3f51):
        p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=end, end=end)
    got = {}
    for label, flags in (('live', 0x1000000), ('dying', 0x1001000), ('freed', 0)):
        u = units + 0x138
        p.uc.mem_write(u, bytes(0x138)); p.uc.mem_write(tp, bytes(0x300))
        p.uc.mem_write(u + 0x130, struct.pack('<I', flags)); p.uc.mem_write(u + 0xb4, struct.pack('<I', tp))
        p.uc.mem_write(tp + 0x1be, struct.pack('<I', 200)); p.uc.mem_write(u + 0x10c, struct.pack('<H', 7))
        p.uc.mem_write(args, struct.pack('<HHi', 1, 0, 50))
        frame = STACK + STACK_SZ - 0x3000
        p.uc.mem_write(frame + 0xc, struct.pack('<II', 2, args)); p.uc.mem_write(frame - 0x1c, struct.pack('<I', 2))
        p.uc.reg_write(UC_X86_REG_EBP, frame); p.uc.reg_write(UC_X86_REG_ESP, frame - 0x400)
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        p.uc.emu_start(0x4d3dd0, 0x4d3f59, count=500)
        got[label] = struct.unpack('<H', p.uc.mem_read(u + 0x10c, 2))[0]
    assert got == {'live': 100, 'dying': 100, 'freed': 7}, got
    return '4d3e27: SetAttribute writes a live or dying unit, never a freed one'


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/new_unit_mana_test'
    print(static_writers())
    p = Icd()
    p.hooks[0x51e4d0] = lambda uc, sp: (2, 0)
    p.freeze_hooks()
    print(init_pool(p))
    print(record_defaults())
    print(regeneration(p))
    print(set_attribute_gate())
    print(finished_feature(p))
    print(cloak_order(p))
    print(storage_arguments())
    print(empty_resources(p))
    credit = credit_storage(p)
    rows, expected = [], []
    for flag in (0, 1):
        for storage in (0.0, 100.0, 300.0, 5000.0, 0.5, 333.3):
            for stored, capacity, total in ((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (2799.5, 5000.0, 123.25),
                                            (16777215.0, 3.0, 1e9)):
                rows.append('%d %d %d %d %d' % (flag, *(struct.unpack('<I', struct.pack('<f', v))[0]
                                                        for v in (storage, stored, capacity)),
                                                struct.unpack('<Q', struct.pack('<d', total))[0]))
                expected.append('%d %d %d' % credit(flag, storage, stored, capacity, total))
    binary = sys.argv[1] if len(sys.argv) > 1 else 'build-o2/new_unit_mana_test'
    out = subprocess.run([binary, '--storage'], input='\n'.join(rows) + '\n', text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(out) == len(expected), (len(out), len(expected))
    for row, got, want in zip(rows, out, expected):
        if got.strip() != want:
            raise AssertionError('storage %s\n  port   %s\n  native %s' % (row, got, want))
    print('PASS: %d creation storage credits match retail 5123ce (pool, capacity, lifetime total)' % len(rows))
    placed, scripted = percentages(p)
    rows, expected = [], []
    for maxmana in (0.0, 20.0, 150.0, 1000.0, 5000.0, 333.3, 0.7):
        bits = struct.unpack('<I', struct.pack('<f', maxmana))[0]
        for maxhp in (0, 1, 100, 651, 15750, 65535):
            for pct in (0, 1, 7, 10, 25, 33, 45, 99, 100, 101, 150, 1000):
                a, b = placed(bits, maxhp, pct), scripted(bits, maxhp, pct)
                assert a == b, (maxmana, maxhp, pct, a, b)
                rows.append('%d %d %d' % (bits, maxhp, pct))
                expected.append('%d %d' % a)
    out = subprocess.run([binary, '--native'], input='\n'.join(rows) + '\n', text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(out) == len(expected), (len(out), len(expected))
    for row, got, want in zip(rows, out, expected):
        if got.strip() != want:
            raise AssertionError('%s\n  port   %s\n  native %s' % (row, got, want))
    print('PASS: %d HealthPercentage / ManaPercentage cases match retail 514c9d and SetAttribute 4d3e6a/4d3eb9 '
          '(hit points chopped to a word, mana capped at MaxMana)' % len(rows))


if __name__ == '__main__':
    main()
