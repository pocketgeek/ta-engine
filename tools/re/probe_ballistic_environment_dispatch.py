#!/usr/bin/env python3
"""Run an authored ballistic arrow through native environment impact dispatch.

The retail 0x52bf90 update and 0x52a4d0 collision run unmodified. Only map-cell
lookup is a controlled adapter and 0x529c10 is intercepted after the real
collision result so the probe can verify its damage-dispatch arguments without
building retail unit, sound, and effect tables.
"""
import pathlib
import re
import struct
import subprocess
import sys

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


root = pathlib.Path(__file__).resolve().parents[2]
binary = sys.argv[1] if len(sys.argv) > 1 else "build-o2/retail_visual_test"

# Arabow WEAPON1 is the shipped model-backed standard arrow.
fbi = (root / "assets/extracted/all/units/arabow.fbi").read_text(errors="replace")
match = re.search(r"\[WEAPON1\]\s*\{(.*?)\n\}", fbi, re.S)
assert match, "arabow WEAPON1 is missing"
block = match.group(1)
for key, value in (("type", "ballistic"), ("model", "araarrow"), ("default", "476")):
    pattern = rf"\b{key}\s*=\s*{re.escape(value)}\s*;"
    damage = re.search(r"\[DAMAGE\]\s*\{(.*?)\n\s*\}", block, re.S)
    assert damage, "Arabow WEAPON1 damage block is missing"
    haystack = block if key != "default" else damage.group(1)
    assert re.search(pattern, haystack, re.I), f"Arabow WEAPON1 {key} does not match {value}"

p = Icd()
game, weapon, shot, cells, features, rules, update = [HEAP + i * 0x10000 for i in range(7)]
cell = cells + 14 * 32
map_lookups = []
damage_dispatches = []


def put(address, value):
    p.uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def map_cell(uc, sp):
    point = struct.unpack("<I", uc.mem_read(sp, 4))[0]
    map_lookups.append(struct.unpack("<3i", uc.mem_read(point, 12)))
    return 1, cell


def damage_dispatch(uc, sp):
    args = struct.unpack("<5I", uc.mem_read(sp, 20))
    damage_dispatches.append(args)
    return 5, 0


p.hooks.update({0x50E660: map_cell, 0x529C10: damage_dispatch})
p.freeze_hooks()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)

# One occupied cell is a 10-unit minimum-height ground cell with an authored
# 50-unit feature top. A 60-unit-high arrow contacts that top inclusively.
put(0x62D55C, game)
put(game + 0x19EC0, 3)
put(game + 0x19EDC, features)
put(game + 0x19E98, 8)
put(game + 0x175DC, rules)
put(game + 0x19ECC, 0)  # isolate collision from gravity
put(rules + 0xD3D, 0)
p.uc.mem_write(game + 0x19EF8, bytes((255,)))
p.uc.mem_write(cell, bytes(14))
p.uc.mem_write(cell + 5, bytes((10, 10)))
p.uc.mem_write(cell + 8, struct.pack("<H", 0))  # feature type zero
p.uc.mem_write(features + 0x138, bytes((50,)))

# Native BallisticWeapon::update: one horizontal substep enters the feature top.
put(shot, weapon)
p.uc.mem_write(shot + 4, struct.pack("<3i", 32 * 65536, 60 * 65536, 32 * 65536))
p.uc.mem_write(shot + 0x1C, struct.pack("<3i", 65536, 0, 0))
p.uc.mem_write(shot + 0x34, bytes(6))
put(shot + 0x20, 0)
put(shot + 0x80, 0)
put(weapon + 0xC8, 0)  # ordinary arrow: no units-only, bounce, or water bypass
put(weapon + 0xD0, 1)
p.uc.mem_write(weapon + 0xB8, bytes(6))
put(update + 8, struct.unpack("<I", struct.pack("<f", 1.0))[0])

_, error = p.call(0x52BF90, (shot,), ecx=update)
assert error is None, error
assert len(map_lookups) == 1, map_lookups
assert len(damage_dispatches) == 1, damage_dispatches
assert damage_dispatches[0] == (shot, 0, 1, 1, 0), damage_dispatches
impact_point = struct.unpack("<3i", p.uc.mem_read(shot + 4, 12))
assert impact_point == (33 * 65536, 60 * 65536, 32 * 65536), impact_point

# The World helper agrees on the same environmental predicate and inputs. The
# live World ballistic update is separately checked by its focused regression.
rows = f"{impact_point[1]} 0 0 10 255 50 0\n"
result = subprocess.run([binary, "--projectile-environment"], input=rows,
                        text=True, capture_output=True, check=True)
assert result.stdout.strip() == "1 0", result.stdout

print("PASS: Arabow's authored ballistic arrow hits the native feature top and dispatches one environment damage event with no unit target; World environmental predicate agrees")
