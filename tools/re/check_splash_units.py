#!/usr/bin/env python3
"""Verify the unit side of retail's area damage, 529dc0, against the port.

Each random case lays out a small map of 14-byte cells whose words +0
(ground) and +2 (airborne) name units, gives every unit its own position and
bounding box (type +13a..+14e), and runs native 529dc0 with a damage callback
that records each recipient and its fraction. The units-only weapon flag
(+c8 0x800) skips the feature half. The cell window, map clamping, the owner
exclusion (+7c), the 20-unit memory, liveness (+130 bit 0x1000000), the 3D box
distance, its x87 square root and truncation and the falloff all execute
natively; the cell lookup (50e600) and the owner's score update (4099e0) are
supplied. The same layouts go through tak::sim::retailSplashUnits (the routine
World::applyHit runs) via retail_visual_test --splash-units, and every
recipient, in order, and every fraction's bits must match.

    python3 tools/re/check_splash_units.py [build-o2/retail_visual_test]
"""
import random
import struct
import subprocess
import sys
from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW

STRIDE = 0x138
GAME, SHOT, WEAPON, CALLBACK, SCORE = (HEAP + o for o in (0, 0x20000, 0x21000, 0x22000, 0x23000))
UNITS, TYPES, CELLS = HEAP + 0x30000, HEAP + 0x40000, HEAP + 0x50000
MAX_UNITS = 63

p = Icd()
uc = p.uc
geometry = {"width": 1}
hits = []


def put(address, value, fmt="<I"):
    uc.mem_write(address, struct.pack(fmt, value))


def cell_pointer(_uc, sp):
    x, z = struct.unpack("<ii", uc.mem_read(sp, 8))
    return 2, CELLS + (z * geometry["width"] + x) * 14


def callback(_uc, sp):
    _, unit, fraction = struct.unpack("<3I", uc.mem_read(sp, 12))
    hits.append(((unit - UNITS) // STRIDE, fraction))
    return 3, 0


p.hooks.update({0x50E600: cell_pointer, CALLBACK: callback, 0x4099E0: lambda _uc, sp: (3, 0)})
p.freeze_hooks()
uc.reg_write(UC_X86_REG_FPCW, 0x027F)
put(0x62D55C, GAME)
put(GAME + 0x14E84, UNITS)
put(WEAPON + 0xC8, 0x800)   # unitsonly: the feature half is skipped
put(SHOT, WEAPON)
uc.mem_write(SHOT + 0x92, b"\x01")


def fixed(rng, low, high):
    return rng.randint(low, high - 1) * 65536 + rng.randrange(65536)


def case(rng):
    width, height = rng.randint(1, 40), rng.randint(1, 40)
    aoe = rng.choice((0, 1, 2, 15, 16, 17, 31, 32, 33, 47, 48, 49, 100, 203, 500, 700,
                      rng.randrange(1024), rng.randrange(65536)))
    radius = aoe >> 1
    edge = rng.choice((0.0, 1.0, 0.1, 0.5, 0.25, 0.3, rng.random(), rng.uniform(-1.0, 2.0)))
    edge_bits = struct.unpack("<I", struct.pack("<f", edge))[0]
    impact = (fixed(rng, -64, width * 16 + 64), fixed(rng, -50, 400), fixed(rng, -64, height * 16 + 64))
    count = rng.choice((0, 1, 3, 8, 19, 20, 21, 30, 45, 60))
    reach = min(radius, 400) + 64
    units = []
    for unit_id in range(1, count + 1):
        live = int(rng.random() < 0.9)
        origin = (impact[0] + fixed(rng, -reach, reach),
                  impact[1] + rng.choice((0, fixed(rng, -40, 40), fixed(rng, 0, 300))),
                  impact[2] + fixed(rng, -reach, reach))
        if rng.random() < 0.8:   # the shape every unit type has: +-foot*8, 0..top
            fx, fz = rng.randint(0, 6), rng.randint(0, 6)
            low = (-(fx << 19), 0, -(fz << 19))
            high = (fx << 19, rng.choice((0, fixed(rng, 0, 100))), fz << 19)
        else:
            low = tuple(-fixed(rng, 0, 80) for _ in range(3))
            high = tuple(fixed(rng, -10, 80) for _ in range(3))
        units.append((unit_id, live, origin, low, high))
    words = {}
    for unit_id, _, origin, _, _ in units:
        cx, cz = (origin[0] >> 20), (origin[2] >> 20)
        for _ in range(rng.randint(1, 4)):
            x, z = cx + rng.randint(-1, 1), cz + rng.randint(-1, 1)
            if 0 <= x < width and 0 <= z < height:
                ground, air = words.get((x, z), (0, 0))
                if rng.random() < 0.5:
                    ground = unit_id
                else:
                    air = unit_id
                words[(x, z)] = (ground, air)
    for _ in range(rng.randint(0, 6)):   # 0xffff in either word
        x, z = rng.randrange(width), rng.randrange(height)
        ground, air = words.get((x, z), (0, 0))
        words[(x, z)] = (-1, air) if rng.random() < 0.3 else (ground, -1)
    owner = rng.choice((0, 0, rng.randint(1, count) if count else 0, count + 1))
    return width, height, aoe, edge_bits, impact, units, words, owner


def native(width, height, aoe, edge_bits, impact, units, words, owner):
    geometry["width"] = width
    put(GAME + 0x19E98, width)
    put(GAME + 0x19E9C, height)
    put(GAME + 0x14E88, UNITS + MAX_UNITS * STRIDE)
    uc.mem_write(UNITS, bytes(STRIDE * (MAX_UNITS + 1)))
    uc.mem_write(CELLS, bytes(width * height * 14))
    for unit_id, live, origin, low, high in units:
        unit, kind = UNITS + unit_id * STRIDE, TYPES + unit_id * 0x200
        put(unit + 0x130, 0x01000000 if live else 0)
        put(unit + 0xB4, kind)
        put(unit + 0xB8, SCORE)
        uc.mem_write(unit + 0x68, struct.pack("<3i", *origin))
        uc.mem_write(kind + 0x13A, struct.pack("<3i", *low))
        uc.mem_write(kind + 0x146, struct.pack("<3i", *high))
    if owner:   # 529dc0 ends by crediting the owner's player (4099e0)
        put(UNITS + owner * STRIDE + 0xB8, SCORE)
    for (x, z), (ground, air) in words.items():
        uc.mem_write(CELLS + (z * width + x) * 14, struct.pack("<HH", ground & 0xFFFF, air & 0xFFFF))
    put(WEAPON + 0x8A, aoe, "<H")
    put(WEAPON + 0x8C, edge_bits)
    uc.mem_write(SHOT + 4, struct.pack("<3i", *impact))
    put(SHOT + 0x7C, UNITS + owner * STRIDE if owner else 0)
    hits.clear()
    _, error = p.call(0x529DC0, (SHOT, CALLBACK))
    assert error is None, error
    return "%d%s" % (len(hits), "".join(" %d:%08x" % hit for hit in hits))


def row(width, height, aoe, edge_bits, impact, units, words, owner):
    lines = ["%d %d %d %d %d %d %d %d %d %d" % (width, height, aoe, edge_bits, owner, *impact,
                                                 len(units), len(words))]
    for unit_id, live, origin, low, high in units:
        lines.append("%d %d %s" % (unit_id, live, " ".join(map(str, (*origin, *low, *high)))))
    for (x, z), (ground, air) in words.items():
        lines.append("%d %d %d %d" % (x, z, ground, air))
    return "\n".join(lines)


def main():
    rng = random.Random(0x529DC0)
    cases = [case(rng) for _ in range(4000)]
    expected = [native(*c) for c in cases]
    binary = sys.argv[1] if len(sys.argv) > 1 else "build-o2/retail_visual_test"
    result = subprocess.run([binary, "--splash-units"], input="\n".join(row(*c) for c in cases) + "\n",
                            text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for i, (a, e) in enumerate(zip(actual, expected)):
        assert a == e, (i, cases[i][:5], "port", a, "native", e)
    recipients = sum(int(e.split()[0]) for e in expected)
    partial = sum(1 for e in expected for hit in e.split()[1:] if hit.split(":")[1] not in ("3f800000",))
    print("PASS: %d native 529dc0 unit passes (%d recipients, %d below full damage) match "
          "retailSplashUnits: order, owner, 20-unit memory, 3D box distance and falloff bits"
          % (len(cases), recipients, partial))


if __name__ == "__main__":
    main()
