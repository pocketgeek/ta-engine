#!/usr/bin/env python3
"""Check the campaign `a X Y` clause against retail's mission-order parser.

Runs retail's 0x513fb0(unit, string, table) on order strings with an `a X Y`
clause (and `m X Y` beside it), with the order resolver 0x4de530 and the
mission push 0x4d78a0 hooked to record their arguments and to answer an order
or none as the case says, and the descriptor ctor 0x4d4bf0 hooked to record the
descriptor name (MAKESELECTABLE); the CRT's sscanf is stood in for (its
float conversion needs the CRT start-up). Retail's own code runs the clause cut
(strcspn), the letter dispatch, the ftol scale by the
scenario field +0xdc5 and the end-of-parse selectable rule (514a7d).

It asserts the rule MissionScript::applyOrders ports (src/sim/mission.cpp):

- the point is (ftol(X*65536) * scale, 0, ftol(Y*65536) * scale), scale 16
  for an .ota with [Map Data] (4c86eb) -- X*16, Y*16 px, no half cell;
- the resolver is asked for kind 3 with no target unit; when it gives an
  order the order is pushed with the queue flag 1 at that point;
- when it gives none nothing is pushed and the next clause still reads;
- `m X Y` (kind 2), `p X Y [T]` (kind 9) and `u X Y` (kind 5) take the same
  X*16, Y*16 point;
- MAKESELECTABLE is appended only when some clause pushed and no clause was
  an `a X Y` that got an order (the flag is never cleared, so ANY such clause,
  not the last, decides).

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_mission_attack_point.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP

GS, SCEN, UNIT, TEXT = HEAP, HEAP + 0x20000, HEAP + 0x30000, HEAP + 0x40000
GIVEN = 0x2a           # any non-zero mission byte from the resolver


def retail(order, scale, answers):
    """answers: kind -> whether the resolver gives an order."""
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    def geti(a): return struct.unpack('<i', uc.mem_read(a, 4))[0]
    put(0x62d55c, GS); put(GS + 0x175dc, SCEN)
    uc.mem_write(SCEN + 0xdc5, struct.pack('<I', scale))
    put(UNIT + 0x130, 0x1000020)
    uc.mem_write(TEXT, order.encode() + b'\0')
    trace = []
    def resolve(uc, args):
        out, kind, unit, target, point, extra = (get(args + 4 * i) for i in range(6))
        x, y, z = (geti(point + 4 * i) for i in range(3))
        given = answers.get(kind, False)
        trace.append(('resolve', kind, unit == UNIT, target, x, y, z))
        uc.mem_write(out, bytes((GIVEN if given else 0,)))
        return 6, out
    def push(uc, args):
        mission, queue, unit, target, point = (get(args + 4 * i) for i in range(5))
        where = tuple(geti(point + 4 * i) for i in range(3)) if point else None
        trace.append(('push', mission & 0xff, queue, unit == UNIT, target, where))
        return 11, 0
    def descriptor(uc, args):
        name = bytes(uc.mem_read(get(args), 32)).split(b'\0')[0].decode()
        trace.append(('descriptor', name))
        uc.mem_write(uc.reg_read(__import__('unicorn').x86_const.UC_X86_REG_ECX), b'\x77')
        return 1, 0
    def scan(uc, args):
        # The CRT's sscanf (cdecl): its float conversion goes through pointers
        # the CRT start-up fills (not run here), so the " %f %f" read is done
        # here, as C reads it: leading blanks, the numbers (two, or three for
        # p's time), the count read.
        text = bytes(uc.mem_read(get(args), 256)).split(b'\0')[0].decode()
        fmt = bytes(uc.mem_read(get(args + 4), 16)).split(b'\0')[0]
        if fmt not in (b' %f %f', b' %f %f %f'): raise AssertionError(('format', fmt))
        count = 0
        wanted = fmt.count(b'%')
        for word, out in zip(text.split()[:wanted], [get(args + 8 + 4 * k) for k in range(wanted)]):
            try: value = float(word.rstrip(','))
            except ValueError: break
            uc.mem_write(out, struct.pack('<f', value)); count += 1
        return 0, count
    icd.hooks[0x5d46f0] = scan
    icd.hooks[0x4de530] = resolve
    icd.hooks[0x4d78a0] = push
    icd.hooks[0x4d4bf0] = descriptor
    _, error = icd.call(0x513fb0, (UNIT, TEXT, 0))
    if error: raise AssertionError(error)
    return trace, get(UNIT + 0x130)


def point(x, y, scale):
    return (int(x * 65536.0) * scale, 0, int(y * 65536.0) * scale)


def expect(order, scale, answers, want_trace, selectable_cleared):
    got, bits = retail(order, scale, answers)
    if got != want_trace:
        raise AssertionError({'order': order, 'scale': scale, 'retail': got, 'expected': want_trace})
    if bool(bits & 0x20) == selectable_cleared:
        raise AssertionError({'order': order, 'selectable bit': hex(bits)})


def main():
    cases = 0
    for scale in (16, 1):
        for x, y in ((110, 20), (124, 98), (1.5, 2.25), (0, 0)):
            p = point(x, y, scale)
            # The order given: pushed queued at the point, no MAKESELECTABLE.
            expect(f'a {x} {y}', scale, {3: True},
                   [('resolve', 3, True, 0, *p), ('push', GIVEN, 1, True, 0, p)], True)
            # None given: nothing pushed, the selectable bit left alone.
            expect(f'a {x} {y}', scale, {3: False}, [('resolve', 3, True, 0, *p)], False)
            cases += 2
    a, m = point(5, 5, 16), point(9, 9, 16)
    # A refused `a X Y` drops only itself: the move still goes in, and since
    # no attack on the ground was pushed MAKESELECTABLE follows it.
    expect('a 5 5, m 9 9', 16, {2: True, 3: False},
           [('resolve', 3, True, 0, *a), ('resolve', 2, True, 0, *m), ('push', GIVEN, 1, True, 0, m),
            ('descriptor', 'MAKESELECTABLE'), ('push', 0x77, 1, True, 0, None)], True)
    # Any `a X Y` that got an order holds MAKESELECTABLE back, first or last.
    for order, first in (('a 5 5, m 9 9', 'a'), ('m 9 9, a 5 5', 'm')):
        moves = [('resolve', 2, True, 0, *m), ('push', GIVEN, 1, True, 0, m)]
        attack = [('resolve', 3, True, 0, *a), ('push', GIVEN, 1, True, 0, a)]
        expect(order, 16, {2: True, 3: True}, attack + moves if first == 'a' else moves + attack, True)
    # A lone move gets MAKESELECTABLE (the control for the two above).
    expect('m 9 9', 16, {2: True},
           [('resolve', 2, True, 0, *m), ('push', GIVEN, 1, True, 0, m),
            ('descriptor', 'MAKESELECTABLE'), ('push', 0x77, 1, True, 0, None)], True)
    cases += 4
    # m, p and u take the same point (514120 kind 2, 514307 kind 9, 5141d3
    # kind 5): X*scale, Y*scale, no half cell either. A patrol holds
    # MAKESELECTABLE back (51443f); a move and an unload do not.
    for x, y in ((9, 9), (1.5, 2.25)):
        q = point(x, y, 16)
        selectable = [('descriptor', 'MAKESELECTABLE'), ('push', 0x77, 1, True, 0, None)]
        for order, kind, tail in ((f'm {x} {y}', 2, selectable), (f'u {x} {y}', 5, selectable),
                                  (f'p {x} {y}', 9, []), (f'p {x} {y} 5', 9, [])):
            expect(order, 16, {kind: True}, [('resolve', kind, True, 0, *q), ('push', GIVEN, 1, True, 0, q)] + tail,
                   True)
            cases += 1
    print(f'PASS: {cases} retail parser cases: a X Y is kind 3 at X*scale, Y*scale (no half cell), '
          'queued, a refusal drops only itself, any given a X Y holds MAKESELECTABLE back; '
          'm, p and u take X*16, Y*16 too')


if __name__ == '__main__': main()
