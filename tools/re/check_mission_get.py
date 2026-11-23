#!/usr/bin/env python3
"""Check the mission script's GET table (4d4060) natively, against the port.

A mission script's GET (opcode 0x10043000, valId and four arguments) and its
GET_UNIT_VALUE (0x10042000, valId alone) both call the mission host's vtable
slot +0x54 (56d0cd..56d0e5: GET_UNIT_VALUE pushes its valId with four zeros),
which for the mission host (vtable 5f2774) is 4d4060 (S, the pointer at
5f27c8). This script

  * reads that wiring (S);
  * runs 4d4060 (E) on a fake game state: player records at game+0x2404
    (stride 0x110, population word +0xe8, score +0xa4), the unit cap word
    game+0x14e7c and the unit array game+0x14e84..+0x14e88 (stride 0x138),
    for the ids the shipped scripts use (5, 7, 30, 31, 35, 36, 40), with live
    and dead objects and the zero id, and requires what MissionScript::getValue
    computes:
      - 5: the population word of player record a1 (counted from creation,
        512334 / 512574, to retirement, 512e52), a1 the same 0-based index
        Create's player argument is (4d36cc);
      - 7 (and GET_UNIT_VALUE 7): the unit cap word, whatever the arguments --
        every shipped use tests `population < cap - n` before creating n units;
      - 30: the type word of a live object (+0x130 & 0x1000000), else 0;
      - 31: the owner byte +0xfd of a live object, else 10 (a zero id too);
      - 35 / 36: the footprint-origin cell words +0x74 / +0x76 of a live
        object, sign-extended, else 0;
      - 40: player record a1's score (+0xa4).

    python3 tools/re/check_mission_get.py
"""
import random
import struct
import sys

from emu import Icd, HEAP

GAME = HEAP + 0x1000
UNITS = HEAP + 0x40000
NUNITS = 64


def wiring(icd):
    data = icd.uc.mem_read(0x5f27c8, 4)
    assert struct.unpack('<I', data)[0] == 0x4d4060, 'mission host vtable +0x54 is not 4d4060'
    code = bytes(icd.uc.mem_read(0x56d0cd, 0x1b))
    # mov eax,[esi+8]; push 0 x3; mov ecx,[esi+eax*4+0x24]; dec eax; mov [esi+8],eax;
    # mov eax,[edi]; push 0; push ecx; mov ecx,edi; call [eax+0x54]
    assert code[3:9] == b'\x6a\x00\x6a\x00\x6a\x00', code.hex()
    assert bytes(icd.uc.mem_read(0x56d0e5, 3)) == b'\xff\x50\x54'
    return 'GET and GET_UNIT_VALUE both call host slot +0x54, 4d4060 for a mission (GET_UNIT_VALUE with four zeros)'


def model(state, val, a1):
    pop, score, cap, units = state
    if val == 5:
        return pop[a1]
    if val == 7:
        return cap
    if val == 40:
        return score[a1]
    uid = a1 & 0xffff
    live = uid != 0 and uid < NUNITS and units[uid]['live']
    if val == 30:
        return units[uid]['type'] if live else 0
    if val == 31:
        return units[uid]['owner'] if live else 10
    if val == 35:
        return units[uid]['ox'] if live else 0
    if val == 36:
        return units[uid]['oz'] if live else 0
    raise ValueError(val)


def main():
    icd = Icd()
    print('S', wiring(icd))
    rng = random.Random(12)
    uc = icd.uc
    uc.mem_write(0x62d55c, struct.pack('<I', GAME))
    bad = cases = 0
    for trial in range(300):
        pop = [rng.randrange(0, 600) for _ in range(10)]
        score = [rng.randrange(-5000, 50000) for _ in range(10)]
        cap = rng.choice((20, 150, 200, 250, 500))
        units = []
        for i in range(NUNITS):
            units.append({'live': rng.random() < 0.7, 'type': rng.randrange(1, 400),
                          'owner': rng.randrange(0, 10), 'ox': rng.randrange(-3, 300),
                          'oz': rng.randrange(-3, 300)})
        uc.mem_write(GAME, b'\0' * 0x20000)
        for p in range(10):
            rec = GAME + 0x2404 + p * 0x110
            uc.mem_write(rec + 0xe8, struct.pack('<H', pop[p]))
            uc.mem_write(rec + 0xa4, struct.pack('<i', score[p]))
        uc.mem_write(GAME + 0x14e7c, struct.pack('<H', cap))
        uc.mem_write(GAME + 0x14e84, struct.pack('<I', UNITS))
        uc.mem_write(GAME + 0x14e88, struct.pack('<I', UNITS + (NUNITS - 1) * 0x138))
        for i, u in enumerate(units):
            base = UNITS + i * 0x138
            uc.mem_write(base, b'\0' * 0x138)
            uc.mem_write(base, struct.pack('<H', u['type']))
            uc.mem_write(base + 0x74, struct.pack('<hh', u['ox'], u['oz']))
            uc.mem_write(base + 0xfd, bytes([u['owner']]))
            flags = (0x1000000 if u['live'] else 0) | rng.choice((0, 0x20, 0x1000))
            uc.mem_write(base + 0x130, struct.pack('<I', flags))
        state = (pop, score, cap, units)
        for val in (5, 7, 30, 31, 35, 36, 40):
            for _ in range(6):
                if val in (5, 40):
                    a1 = rng.randrange(0, 10)
                elif val == 7:
                    a1 = rng.choice((0, 1, 5))
                else:
                    a1 = rng.choice((0, rng.randrange(1, NUNITS)))
                args = (val, a1, rng.choice((0, 3)), 0, 0)
                got, err = icd.call(0x4d4060, args=args)
                assert err is None, err
                got = struct.unpack('<i', struct.pack('<I', got & 0xffffffff))[0]
                want = model(state, val, a1)
                cases += 1
                if got != want:
                    bad += 1
                    if bad < 10:
                        print('MISMATCH val %d a1 %d: native %d, port %d' % (val, a1, got, want))
    print('E 4d4060: %d cases, %d disagree' % (cases, bad))
    if bad:
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
