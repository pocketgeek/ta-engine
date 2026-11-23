#!/usr/bin/env python3
"""Check retail's selectable bit (unit +0x130 & 0x20) against the port.

A unit is made with the bit set (511990: `or edx, 0x4021` at 511b21). Only
three things write it afterwards (S, every write of +0x130 in .text with 0x20
in the mask): the end of the mission-order parser 513fb0 (514a7d..514aba, the
clear), the MakeSelectable mission 401450 (the set), a loaded game's flag
restore (5135ba) and a cheat that sets it on every live unit (424f20, no
direct caller). This script

  * runs 513fb0(unit, string, table) natively over clause strings -- every
    clause letter, with the resolver 4de530 and the name and type lookups
    answering yes or no -- with the push 4d78a0, the descriptor ctor 4d4bf0,
    the embark 51b4f0, the standing order 5198a0 and the build-menu test
    519850 hooked, and the CRT's sscanf stood in for (its conversions need the
    CRT start-up). The rule (E): when any clause pushed a mission the bit is
    cleared, and MAKESELECTABLE is appended unless a patrol, an `a X Y` that
    got an order, an `s` or a `d` was among them (the flag 514a8a reads is
    never cleared during the parse). `s` itself pushes MAKESELECTABLE.
    `i`, `v`, `o` and `c` push nothing; `bw` pushes BuildWeapon without
    counting. SetMission (4d37f6) calls the same parser, so a script's orders
    clear the bit too;
  * runs 401450 (E): the bit is set, 0x2000 cleared, the mission ends (5);
  * runs PARK (407ca0) at stage 1 with a next mission (E): it gives way (5)
    for a live, selectable unit, not for an unselectable or dying one;
  * prints the same table from `mission_orders_test --selectable` (the port,
    MissionScript::applyOrders on a World) and requires it to match.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_mission_selectable.py [build/mission_orders_test]
"""
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

GS, SCEN, UNIT, TEXT = HEAP, HEAP + 0x20000, HEAP + 0x30000, HEAP + 0x40000
NAMED, TYPEID = HEAP + 0x50000, 7
GIVEN = 0x2a


def sscanf(uc, args, get):
    """The CRT's sscanf for the parser's formats: blanks, %d, %f, %[set]."""
    text = bytes(uc.mem_read(get(args), 256)).split(b'\0')[0].decode()
    fmt = bytes(uc.mem_read(get(args + 4), 64)).split(b'\0')[0].decode()
    outs = [get(args + 8 + 4 * k) for k in range(8)]
    i, count = 0, 0
    for tok in re.findall(r'%\[[^\]]*\]|%[df]', fmt):
        while i < len(text) and text[i].isspace(): i += 1
        if tok == '%d':
            m = re.match(r'[-+]?\d+', text[i:])
            if not m: break
            uc.mem_write(outs[count], struct.pack('<i', int(m.group())))
        elif tok == '%f':
            m = re.match(r'[-+]?(\d+\.?\d*|\.\d+)', text[i:])
            if not m: break
            uc.mem_write(outs[count], struct.pack('<f', float(m.group())))
        else:
            m = re.match(r'[A-Za-z0-9_.]+' if '_' in tok else r'[A-Za-z0-9.]+', text[i:])
            if not m: break
            uc.mem_write(outs[count], m.group().encode() + b'\0')
        i += m.end(); count += 1
    return 0, count


def retail(order, answer):
    """Parse `order`; resolver, name and type lookups answer `answer`. Returns
    (bit 0x20 after the parse, number of missions pushed, MAKESELECTABLE appended)."""
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    put(0x62d55c, GS); put(GS + 0x175dc, SCEN)
    uc.mem_write(SCEN + 0xdc5, struct.pack('<I', 16))
    put(UNIT + 0x130, 0x1000021)
    put(UNIT + 0xb4, HEAP + 0x60000)     # its type: cloakcost (+0x19a) 0
    uc.mem_write(TEXT, order.encode() + b'\0')
    pushed, names = [], []
    def resolve(uc_, args):
        out = get(args)
        uc.mem_write(out, bytes((GIVEN if answer else 0,)))
        return 6, out
    def descriptor(uc_, args):
        names.append(bytes(uc.mem_read(get(args), 32)).split(b'\0')[0].decode())
        uc.mem_write(uc.reg_read(UC_X86_REG_ECX), b'\x77')
        return 1, 0
    def push(uc_, args):
        # The parser's own append returns to 514abf; an `s` clause's push does not.
        pushed.append('append' if get(args - 4) == 0x514abf else
                      names[-1] if names and (get(args) & 0xff) == 0x77 else 'resolved')
        return 11, 0
    icd.hooks.update({
        0x5d46f0: lambda uc_, a: sscanf(uc_, a, get),
        0x4de530: resolve, 0x4d78a0: push, 0x4d4bf0: descriptor,
        0x513ec0: lambda uc_, a: (0, NAMED if answer else 0),        # a placed unit by name
        0x515ac0: lambda uc_, a: (0, TYPEID if answer else 0),       # a unit type by name
        0x519850: lambda uc_, a: (0, 1),                             # on the build menu
        0x51b4f0: lambda uc_, a: (0, 1),                             # embark
        0x5198a0: lambda uc_, a: (0, 0),                             # standing order
    })
    _, error = icd.call(0x513fb0, (UNIT, TEXT, 0))
    if error: raise AssertionError((order, error))
    assert 'append' not in pushed[:-1], pushed
    tail = bool(pushed) and pushed[-1] == 'append' and names[-1] == 'MAKESELECTABLE'
    return bool(get(UNIT + 0x130) & 0x20), len(pushed) - int(tail), tail


def make_selectable():
    icd = Icd(); uc = icd.uc
    uc.mem_write(UNIT + 0x130, struct.pack('<I', 0x1002001))
    ret, error = icd.call(0x401450, (UNIT, 0, 0))
    assert not error, error
    bits = struct.unpack('<I', uc.mem_read(UNIT + 0x130, 4))[0]
    return ret, bits


def park_yields(flags):
    """PARK (407ca0) at stage 1 with a next mission (+0x66), no events: 5 when it
    gives way, else it goes on to its retries (stopped at 407d61)."""
    from unicorn import UC_HOOK_CODE
    icd = Icd(); uc = icd.uc
    mission = HEAP + 0x70000
    uc.mem_write(UNIT, bytes(0x138)); uc.mem_write(mission, bytes(0x80))
    uc.mem_write(UNIT + 0x130, struct.pack('<I', flags))
    uc.mem_write(mission + 5, bytes((1,))); uc.mem_write(mission + 0x66, struct.pack('<I', 1))
    uc.hook_add(UC_HOOK_CODE, lambda u, *_: u.emu_stop(), begin=0x407d61, end=0x407d61)
    ret, error = icd.call(0x407ca0, (UNIT, mission, 0), allow_early_stop=True)
    assert not error, error
    return ret == 5 and uc.reg_read(__import__('unicorn').x86_const.UC_X86_REG_EIP) != 0x407d61


# Each clause with its own answers: (text, resolver/lookups say yes?)
CLAUSES = ['m 5 7', 'ma 5 7', 'u 5 7', 'p 5 7', 'p 5 7 9', 'a 5 7', 'a NAMEU', 'g NAMEU', 'w 30', 'w 60 20',
           'wa', 'wa NAMEU', 'b ARATYPE 2', 'b ARATYPE 1 5 7', 's', 'd', 'i NAMEU', 'v 1.5', 'o 2', 'o 1 2', 'c',
          ]
ORDERS = [c for c in CLAUSES] + ['w 30, m 5 7', 'm 5 7, p 9 9', 's, m 5 7', 'm 5 7, s', 'i NAMEU, m 5 7',
                                 'v 1.0, o 2', 'w 30, d', 'a 5 7, m 9 9', 'o 2, wa, ', 'c, w 30']


def table():
    rows = []
    for order in ORDERS:
        for answer in (True, False):
            sel, n, tail = retail(order, answer)
            rows.append(f'{order!r} {int(answer)} selectable={int(sel)} pushed={n} tail={int(tail)}')
    return rows


def main():
    ret, bits = make_selectable()
    assert ret == 5 and bits & 0x20 and not bits & 0x2000, (ret, hex(bits))
    # PARK gives way to a next mission only for a selectable unit (407d1e).
    assert park_yields(0x1000020) and not park_yields(0x1000000) and not park_yields(0x1001020)
    rows = table()
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'mission_orders_test')
    ours = subprocess.run([binary, '--selectable'], capture_output=True, text=True, check=True).stdout.split('\n')
    ours = [line for line in ours if line.startswith("'")]
    want = [r.split(' pushed=')[0] + ' tail=' + r.split(' tail=')[1] for r in rows]
    if want != ours:
        for r, w, o in zip(rows, want, ours + [''] * len(rows)):
            print(('   ' if w == o else 'XX ') + r + '   | ours: ' + o)
        raise SystemExit('FAIL: the port differs from retail')
    print(f'PASS: 401450 sets 0x20 and clears 0x2000 (returns 5); PARK gives way only for a selectable '
          f'unit; {len(rows)} parser cases: a pushed mission '
          'clears the selectable bit, MAKESELECTABLE follows unless a patrol, a given a X Y, s or d was '
          'among the clauses; the port matches')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--retail':
        print(make_selectable())
        print('\n'.join(table()))
    else:
        main()
