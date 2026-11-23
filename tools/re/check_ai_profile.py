#!/usr/bin/env python3
"""Compare the Classic AI profile baker with retail's own.

Random AI-profile scripts run through retail's script executor 0x538f00 with
its weight/limit handlers (0x409760 -> 0x410c80, 0x4097f0 -> 0x410d50)
registered by 0x4098e0, over a synthetic unit-type table. The resulting
preference bytes, limits and both lock arrays are compared with
`classic_ai_test --profile` (src/ai/classicprofile.cpp) on the same scripts.

Only the category lookup (0x515c40, a std::map the FBI loader fills) and the
allocator are stand-ins: the lookup answers from this script's own table, as
the loader would have built it. Tokenizing, number reading, name resolution,
locking and the stores run in the original code.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

NAMES = ['araarch', 'arabuild', 'aracastl', 'aradrag', 'arakeep', 'araking', 'aralode',
         'aramana', 'arasiege', 'tararch', 'tarlode', 'tarnecro', 'zonmana', 'cregate']
CATEGORIES = ['ara', 'tar', 'zon', 'cre', 'lodestone', 'builder', 'ship']
VALUES = ['0', '1', '2', '5', '25', '200', '0.5', '0.999', '0.019', '0.0099', '-1', '-0.5',
          '1.9', '.25', '3.', '1e2', '1E-2', '2d1', '1e', '+4', '7x', 'abc', '', '0.29',
          '0.07', '0.333', '99.99', '1e9', '1e40', '42949673', '67108864', '4294967297',
          '-2147483648', '12abc', ' ', '%1', '0x10']
COMMANDS = ['weight', 'WEIGHT', 'Weight', 'limit', 'LIMIT', 'Limit', 'plan', 'bogus', '//', '//weight']


def names_and_categories(rng):
    names = sorted(rng.sample(NAMES, rng.randrange(4, len(NAMES) + 1)))
    cats = {n: rng.sample(CATEGORIES, rng.randrange(0, 3)) for n in names}
    return names, cats


def random_case(rng, word, value):
    token = lambda t: ''.join(c.upper() if rng.random() < 0.3 else c for c in t)
    space = lambda: rng.choice([' ', '  ', '\t', ' \t', '\r'])
    lines = []
    for _ in range(rng.randrange(1, 14)):
        r = rng.random()
        if r < 0.05:
            lines.append(rng.choice(['', '   ', '// comment', '# comment', '\r']))
            continue
        parts = [rng.choice(COMMANDS)]
        if rng.random() < 0.95:
            parts.append(token(word()))
        if rng.random() < 0.9:
            parts.append(value())
        if rng.random() < 0.1:
            parts.append(rng.choice(['extra', '# 5', '//', '9']))
        line = space().lstrip('\r') if rng.random() < 0.2 else ''
        line += space().join(parts)
        if rng.random() < 0.05:
            cut = rng.randrange(len(line) + 1)
            line = line[:cut] + '#' + line[cut:]
        lines.append(line)
    ending = rng.choice(['\n', '\r\n'])
    text = ending.join(lines) + (ending if rng.random() < 0.8 else '')
    return text.encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=1500)
    ap.add_argument('--catalogues', type=int, default=8)
    args = ap.parse_args()
    p = Icd()
    uc = p.uc
    # The C runtime's start-up control word (53-bit precision), as the other
    # x87 oracles select. Unicorn's own default rounds the weight product to
    # single precision, which turns 100 x 0.29f (28.99999917) into 29.
    uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    bump = [HEAP + 0x300000]

    def alloc(uc_, sp):
        n = struct.unpack('<I', uc_.mem_read(sp, 4))[0]
        a = bump[0]
        bump[0] += (n + 15) & ~15
        uc_.mem_write(a, b'\0' * n)
        return 0, a
    for address in (0x5ba690, 0x5ba480):
        p.hooks[address] = alloc
    p.hooks[0x5ba5d0] = lambda uc_, sp: (0, 0)
    empty = HEAP + 0x3f0000
    masks = {}

    def cstring(address):
        out = b''
        while True:
            b = uc.mem_read(address + len(out), 1)
            if b == b'\0':
                return out.decode('latin-1')
            out += b

    def category(uc_, sp):
        name = cstring(struct.unpack('<I', uc_.mem_read(sp, 4))[0]).lower()
        return 1, masks.get(name, empty)
    p.hooks[0x515c40] = category
    p.freeze_hooks()
    w32 = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    r32 = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    game, defs, manager = HEAP, HEAP + 0x40000, HEAP + 0x100000
    w32(0x62d55c, game)
    player = game + 0x2404
    w32(player, 1)
    w32(player + 0x80, 1)
    uc.mem_write(player + 0xea, bytes([2]))
    w32(0x62a33c, manager)
    _, error = p.call(0x4098e0)
    if error:
        raise RuntimeError(error)
    # The executor allocates per line and the bump allocator never frees; rewind
    # it for every script, or it grows into the script buffer.
    mark = bump[0]
    rng = random.Random(0x538f00)
    total = 0
    for catalogue in range(args.catalogues):
        names, cats = names_and_categories(rng)
        table = [''] + names
        n = len(table)
        w32(game + 0x175b8, n)
        w32(game + 0x175c4, defs)
        for t, name in enumerate(table):
            d = defs + t * 0x2a4
            uc.mem_write(d + 0x20, name.encode() + b'\0' * 8)
            uc.mem_write(d + 0x80, struct.pack('<H', t))
        masks.clear()
        for i, word in enumerate(CATEGORIES + ['all']):
            bits = bytearray(64)
            for t, name in enumerate(table[1:], 1):
                if word == 'all' or word in cats[name]:
                    bits[t >> 3] |= 1 << (t & 7)
            address = HEAP + 0x3e0000 + i * 0x40
            uc.mem_write(address, bytes(bits))
            masks[word] = address
        for off, size in [(0x69, 1), (0x85, 2), (0x95, 2), (0xa5, 1), (0xb5, 1), (0xc5, 1),
                          (0xd5, 4), (0xe5, 4), (0xf5, 4)]:
            base = manager + 0x1000 + off * 0x40
            w32(manager + off, base)
            w32(manager + off + 4, base + n * size)
            w32(manager + off + 8, base + n * size)
        typo = lambda: rng.choice(['arasiegee', 'aracastle', 'arach', 'foo', 'tarlod'])
        word = lambda: rng.choice([rng.choice(names)] * 6 + [rng.choice(CATEGORIES + ['all', 'ALL'])] * 2 + [typo()])
        value = lambda: rng.choice(VALUES) if rng.random() < 0.8 else f'{rng.uniform(-3, 300):.{rng.randrange(0, 5)}f}'
        rows, expected = [], []
        for _ in range(args.cases // args.catalogues):
            text = random_case(rng, word, value)
            bump[0] = mark
            _, error = p.call(0x410670, ecx=manager)
            if error:
                raise RuntimeError(error)
            buf, ctx = HEAP + 0x380000, HEAP + 0x390000
            uc.mem_write(buf, text)
            w32(ctx + 0x280, 0)
            _, error = p.call(0x538f00, args=(buf, len(text), ctx, -1))
            if error:
                raise RuntimeError(error)
            row = []
            for t in range(1, n):
                row += [uc.mem_read(r32(manager + 0xc5) + t, 1)[0],
                        struct.unpack('<i', uc.mem_read(r32(manager + 0xe5) + 4 * t, 4))[0],
                        r32(r32(manager + 0xd5) + 4 * t), r32(r32(manager + 0xf5) + 4 * t)]
            expected.append(row)
            rows.append(text)
        stdin = f'types {len(names)}\n' + ''.join(f'{name} {" ".join(cats[name])}\n' for name in names)
        stdin += ''.join(f'case {text.hex()}\n' for text in rows)
        proc = subprocess.run([args.binary, '--profile'], input=stdin, text=True,
                              capture_output=True, check=True)
        actual = [list(map(int, line.split())) for line in proc.stdout.splitlines()]
        if len(actual) != len(expected):
            raise AssertionError(f'{len(actual)} rows for {len(expected)} cases')
        for text, want, got in zip(rows, expected, actual):
            if want != got:
                diff = [(table[1 + i // 4], 'pref limit prefLock limitLock'.split()[i % 4], w, g)
                        for i, (w, g) in enumerate(zip(want, got)) if w != g]
                raise AssertionError(f'script {text!r}: (type, field, retail, ours) {diff}')
        total += len(rows)
    print(f'PASS: {total} random profile scripts over {args.catalogues} unit tables '
          '(preference, limit and both locks per type)')


if __name__ == '__main__':
    main()
