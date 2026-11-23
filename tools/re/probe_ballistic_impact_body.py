#!/usr/bin/env python3
"""Exercise retail 0x529c10 impact dispatch without launching the game.

Map/cell lookup, weapon damage lookup, effect queue insertion, and projectile
COM releases use controlled services. Dispatch-only cases also substitute unit
and feature damage callbacks; native_mutation cases run the native unit damage
and HP update path. The feature-threshold replacement case also runs native
feature placement, accumulation, removal, replacement, and piece-tree
clone/teardown. It supplies a synthetic empty leaf piece tree and cdecl heap
shims, while map/terrain services stay controlled; it performs no authored
feature rendering or Glide submission. Retail impact, splash scan, per-hit
damage math, and projectile retirement run from KINGDOMS.icd.
"""
import pathlib
import re
import struct

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_FPCW


ROOT = pathlib.Path(__file__).resolve().parents[2]
GRID = 64
STRIDE = 0x138


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def put16(uc, address, value):
    uc.mem_write(address, struct.pack("<H", value & 0xFFFF))


def putf(uc, address, value):
    uc.mem_write(address, struct.pack("<f", value))


def read32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def shipped_profile(unit, weapon, expected):
    text = (ROOT / f"assets/extracted/all/units/{unit}.fbi").read_text(errors="replace")
    match = re.search(rf"\[WEAPON{weapon}\]\s*\{{(.*?)\n\}}", text, re.I | re.S)
    assert match, f"missing {unit} WEAPON{weapon}"
    block = match.group(1)
    damage = re.search(r"\[DAMAGE\]\s*\{(.*?)\n\s*\}", block, re.I | re.S)
    assert damage, f"missing {unit} WEAPON{weapon} damage table"
    for key, value in expected.items():
        haystack = damage.group(1) if key == "default" else block
        assert re.search(rf"\b{key}\s*=\s*{re.escape(str(value))}\s*;", haystack, re.I), \
            f"{unit} WEAPON{weapon} {key} did not match {value}"
    return block


def make_probe(base_damage, aoe, edge, effect=False, units=(), native_mutation=False,
               native_allocators=False):
    p = Icd()
    uc = p.uc
    names = ("game", "shot", "weapon", "owner", "units", "kind", "app",
             "appcore", "cells", "features", "rules", "vtable", "iface",
             "result", "featurepos", "featuremap")
    addr = {name: HEAP + i * 0x10000 for i, name in enumerate(names)}
    G, S, W, O, U, K, A, AC, C, F, R, VT, IF, RES, FP, FM = (addr[x] for x in names)
    logs = []

    def cell_at(x, z):
        return C + (z * GRID + x) * 14

    def map_cell(emu, sp):
        point = read32(emu, sp)
        x, y, z = struct.unpack("<3i", emu.mem_read(point, 12))
        logs.append(("map", x // 65536, z // 65536))
        return 1, cell_at((x // 65536) // 16, (z // 65536) // 16)

    def map_index(emu, sp):
        x, z = struct.unpack("<ii", emu.mem_read(sp, 8))
        return 2, cell_at(x, z)

    def feature_point(emu, sp):
        emu.mem_write(FP, struct.pack("<3i", 256 * 65536, 10 * 65536, 256 * 65536))
        return 3, FP

    def feature_damage(emu, sp):
        logs.append(("feature-dispatch", struct.unpack("<5I", emu.mem_read(sp, 20))))
        return 5, 0

    def damage_lookup(emu, sp):
        logs.append(("damage-lookup", read32(emu, sp)))
        return 1, base_damage

    def apply_damage(emu, sp):
        args = struct.unpack("<5I", emu.mem_read(sp, 20))
        logs.append(("unit-dispatch", args))
        return 5, 0

    def effect_check(emu, sp):
        logs.append(("effect-check", struct.unpack("<3i", emu.mem_read(read32(emu, sp), 12))))
        return 1, 1

    def effect_dispatch(emu, sp):
        logs.append(("effect-create", struct.unpack("<2I", emu.mem_read(sp, 8))))
        return 2, 0

    def environment_event(emu, sp):
        logs.append(("environment-event", struct.unpack("<3I", emu.mem_read(sp, 12))))
        return 3, 0

    def reference_release(emu, sp):
        logs.append(("release", emu.reg_read(UC_X86_REG_ECX), read32(emu, sp)))
        return 1, 0

    hooks = {
        0x50E660: map_cell, 0x50E600: map_index,
        0x531E50: damage_lookup,
        0x50A7D0: environment_event, 0x4931E0: feature_point,
        0x48C870: effect_check,
        0x48C0B0: effect_dispatch, 0x5D4444: lambda emu, sp: (0, 0),
        0x51F340: lambda emu, sp: (2, 1), 0x4099E0: lambda emu, sp: (3, 0),
        VT + 0x14: lambda emu, sp: (1, 0),
    }
    if not native_mutation:
        hooks[0x51A140] = apply_damage
        hooks[0x4961A0] = feature_damage
    dtor = HEAP + 0xFE000
    hooks[dtor] = reference_release
    if native_allocators:
        # Feature instance setup/destruction allocates and frees native piece
        # arrays. Keep those calls cdecl-correct and inside Unicorn's mapped
        # heap; the engine's Windows process heap is not initialized here.
        allocation = {"next": HEAP + 0x180000}

        def allocate(emu, size):
            size = max(size, 1)
            address = (allocation["next"] + 15) & ~15
            allocation["next"] = address + size
            assert allocation["next"] < HEAP + 0x400000, (hex(address), size)
            emu.mem_write(address, bytes(size))
            logs.append(("native-alloc", address, size))
            return address

        def alloc_tagged(emu, sp):
            return 0, allocate(emu, read32(emu, sp + 4))

        def alloc(emu, sp):
            return 0, allocate(emu, read32(emu, sp))

        def free(emu, sp):
            logs.append(("native-free", read32(emu, sp)))
            return 0, 0

        # These binary routines are cdecl. Returning 0 args from Icd's call
        # shim preserves the caller's cleanup (an stdcall-style pop corrupts
        # the native constructor's stack).
        hooks[0x5BA3D0] = alloc_tagged
        hooks[0x5BA3E0] = alloc
        hooks[0x5BA5D0] = free
    p.hooks.update(hooks)
    p.freeze_hooks()
    uc.reg_write(UC_X86_REG_FPCW, 0x027F)

    put(uc, 0x62D55C, G)
    put(uc, 0x62D558, A)
    put(uc, A, AC)
    put(uc, G + 0x175DC, R)
    put(uc, G + 0x19E98, GRID)
    put(uc, G + 0x19E9C, GRID)
    put(uc, G + 0x19EC0, 10)
    put(uc, G + 0x19EDC, F)
    put(uc, G + 0x19ECC, 0)
    put(uc, G + 0x19F04, C)
    put(uc, G + 0x19E74, FM)
    put(uc, G + 0x14E84, U)
    put(uc, G + 0x14E88, U + 6 * STRIDE)
    put(uc, G + 0x14ED0, 0)
    put(uc, G + 0x14ED4, 0)
    put(uc, G + 0x19E54, 10000)
    put(uc, G + 0x19E58, 10000)
    put(uc, 0x60596C, 1)
    uc.mem_write(G + 0x19EF8, b"\xff")

    for z in range(GRID):
        for x in range(GRID):
            c = cell_at(x, z)
            uc.mem_write(c, bytes(14))
            uc.mem_write(c + 5, b"\x0a\x0a")
            put16(uc, c + 8, 0xFFFF)
    for unit_id, (x, z, player) in enumerate(units, start=1):
        target = U + unit_id * STRIDE
        put16(uc, target + 2, unit_id)
        put(uc, target + 0xB4, K)
        put(uc, target + 0xB8, IF)
        put(uc, target + 0x130, 0x01000000)
        put16(uc, target + 0x10C, 1000)
        putf(uc, target + 0x108, 1.0)
        uc.mem_write(target + 0xFD, bytes((player,)))
        putf(uc, target + 0xE4, 1.0)
        put(uc, target + 0x68, x * 65536)
        put(uc, target + 0x6C, 0)
        put(uc, target + 0x70, z * 65536)
        c = cell_at(x // 16, z // 16)
        put16(uc, c if read32(uc, c) & 0xFFFF == 0 else c + 2, unit_id)
    # Zero-sized unit bounds make splash measurement point-to-point.
    for off in (0x13A, 0x13E, 0x142, 0x146, 0x14A, 0x14E):
        put(uc, K + off, 0)
    put(uc, K + 0x9E, 0)
    put(uc, K + 0x1BE, 1000)
    uc.mem_write(K + 0x268, bytes(4))

    # The native area pass resolves the one feature at the impact tile.
    put16(uc, cell_at(16, 16) + 8, 0)
    put16(uc, cell_at(16, 16) + 10, 0)
    put(uc, O + 0x130, 0x01000000)
    uc.mem_write(O + 0xFD, b"\0")
    putf(uc, O + 0xE0, 1.0)
    put(uc, O + 0xB8, IF)
    put(uc, IF + 0x80, IF)

    # Profile-derived fields used by 0x529c10 / 0x529dc0 / 0x52a330.
    put(uc, W + 0xB0, 0xFFFFFFFF)
    put(uc, W + 0x6C, 0xFFFFFFFF)
    put(uc, W + 0x70, 0xFFFFFFFF)
    put16(uc, W + 0x8A, aoe)
    putf(uc, W + 0x8C, edge)
    put16(uc, W + 0xB4, 0)
    put(uc, W + 0x40, VT)
    put(uc, VT, 0)
    put(uc, VT + 0x14, VT + 0x14)
    put(uc, IF, VT)
    if effect:
        put(uc, W + 0x80, 0x4343)
        put(uc, W + 0x84, 0x4242)
    put(uc, S, W)
    uc.mem_write(S + 4, struct.pack("<3i", 256 * 65536, 10 * 65536, 256 * 65536))
    put(uc, S + 0x7C, O)
    uc.mem_write(S + 0x92, b"\0")
    uc.mem_write(S + 0xD8, bytes(4))
    # Three COM-like references are retired by retail 0x529af0.
    for i, offset in enumerate((0xA8, 0xAC, 0xB0)):
        obj = HEAP + (0xD0000 + i * 0x1000)
        vtable = HEAP + 0xD3000
        put(uc, S + offset, obj)
        put(uc, obj, vtable)
        put(uc, vtable, dtor)
    return p, (G, S, U, STRIDE), logs


def run():
    shipped_profile("arabow", 1, {"type": "ballistic", "model": "araarrow", "default": 476})
    shipped_profile("arapult", 1, {"type": "Ballistic", "areaofeffect": 100,
                                     "edgeeffectiveness": 0.1, "default": 1250})

    p, (_, shot, units, stride), direct = make_probe(476, 0, 1.0, units=((256, 256, 1),))
    victim = units + stride
    _, error = p.call(0x529C10, (shot, victim, 1, 1, 0))
    assert error is None, error
    hit = next(row[1] for row in direct if row[0] == "unit-dispatch")
    assert hit[0] == HEAP + 3 * 0x10000 and hit[1] == victim and hit[2] == 405, hit
    assert read32(p.uc, shot + 0xD8) & 2
    assert len([r for r in direct if r[0] == "release"]) == 3
    print(f"Arabow arrow direct hit: native final damage={hit[2]} from authored 476 (RNG fixed at 0); releases=3; retired=0x{read32(p.uc, shot + 0xD8):x}")

    p, (_, shot, units, stride), direct_native = make_probe(
        476, 0, 1.0, units=((256, 256, 1),), native_mutation=True)
    victim = units + stride
    _, error = p.call(0x529C10, (shot, victim, 1, 1, 0))
    assert error is None, error
    hp = struct.unpack("<h", p.uc.mem_read(victim + 0x10C, 2))[0]
    damage_fraction = struct.unpack("<f", p.uc.mem_read(victim + 0x108, 4))[0]
    assert hp == 595 and abs(damage_fraction - 0.405) < 1e-6, (hp, damage_fraction)
    assert read32(p.uc, victim + 0x130) & 0x01000000
    print(f"Arabow arrow native health mutation: HP 1000→{hp}; damage fraction={damage_fraction:.3f}; unit remains alive")

    p, (_, shot, units, stride), area = make_probe(
        1250, 100, 0.1, effect=True,
        units=((256, 256, 1), (281, 256, 1), (308, 256, 1), (260, 256, 0)))
    _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
    assert error is None, error
    hits = [r[1] for r in area if r[0] == "unit-dispatch"]
    by_id = {((row[1] - units) // stride): row[2] for row in hits}
    assert by_id == {1: 719, 4: 719, 2: 327}, by_id
    assert len([r for r in area if r[0] == "feature-dispatch"]) == 1
    assert len([r for r in area if r[0] == "effect-create"]) == 1
    assert len([r for r in area if r[0] == "release"]) == 3
    assert read32(p.uc, shot + 0xD8) & 2
    print(f"Arapult shell environment impact: native unit damages={by_id} (same-owner unit 4 is included); feature-dispatch=1; effect-create=1; releases=3; retired=0x{read32(p.uc, shot + 0xD8):x}")

    # Direct impact resolves any footprint tail to the head before damage.
    # In particular a zero-AOE bolt/flame cannot disappear into the outer
    # cells of a large tree just because its center is outside the radius.
    for size in (2, 5):
        for aoe in (0, 160):
            p, (_, shot, _, _), tail = make_probe(476, aoe, 1.0)
            cells = HEAP + 8 * 0x10000
            for z in range(size):
                for x in range(size):
                    if not x and not z:
                        continue
                    cell = cells + ((16 + z) * GRID + 16 + x) * 14
                    p.uc.mem_write(cell + 8, struct.pack("<HBB", 0xFFFE, z, x))
            position = (256 + (size - 1) * 16) * 65536
            p.uc.mem_write(shot + 4, struct.pack("<3i", position, 10 * 65536, position))
            _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
            assert error is None, error
            hits = [row[1] for row in tail if row[0] == "feature-dispatch"]
            assert len(hits) == 1 and hits[0][1:3] == (16, 16), (size, aoe, hits)
    print("Feature footprint tails: 2x2/5x5 direct impacts resolve to head exactly once, with and without splash")

    p, (_, shot, units, stride), area_native = make_probe(
        1250, 100, 0.1, effect=True, native_mutation=True,
        units=((256, 256, 1), (281, 256, 1), (308, 256, 1), (260, 256, 0)))
    # Leave the impact tile empty so the retail unit splash scan can be tested
    # independently of the feature damage callback and feature vtable.
    cells = HEAP + 8 * 0x10000
    impact_cell = cells + (16 * GRID + 16) * 14
    put16(p.uc, impact_cell + 8, 0xFFFF)
    put16(p.uc, impact_cell + 10, 0xFFFF)
    _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
    assert error is None, error
    remaining = {i: struct.unpack("<h", p.uc.mem_read(units + i * stride + 0x10C, 2))[0]
                 for i in range(1, 5)}
    assert remaining == {1: 281, 2: 673, 3: 1000, 4: 281}, remaining
    assert len([r for r in area_native if r[0] == "effect-create"]) == 1
    print(f"Arapult native splash health: remaining HP={remaining}; same-owner unit 4 is hit; out-of-radius unit 3 is untouched")

    p, (_, shot, _, _), feature_native = make_probe(
        1250, 100, 0.1, native_mutation=True, units=())
    uc = p.uc
    weapon = HEAP + 2 * 0x10000
    cells = HEAP + 8 * 0x10000
    definitions = HEAP + 9 * 0x10000
    feature_cell = cells + (16 * GRID + 16) * 14
    put16(uc, definitions + 0x126, 5000)
    put16(uc, weapon + 0x88, 1250)
    accumulated = []
    for damage_total in (1250, 2500, 3750):
        put(uc, shot + 0xD8, 0)
        _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
        assert error is None, error
        feature_id = struct.unpack("<H", uc.mem_read(feature_cell + 8, 2))[0]
        damage = struct.unpack("<H", uc.mem_read(feature_cell + 10, 2))[0]
        assert feature_id == 0 and damage == damage_total, (feature_id, damage)
        assert read32(uc, shot + 0xD8) & 2
        accumulated.append(damage)
    print(f"Native feature damage accumulation: {accumulated} below threshold 5000; feature ID unchanged")

    # Native feature destruction must begin from a placed feature instance.
    # A manually seeded cell has no live piece tree and skips the teardown
    # branch in 0x496380, so initialize type 0 with retail's placement routine.
    p, (_, shot, _, _), feature_replace = make_probe(
        1250, 100, 0.1, native_mutation=True, native_allocators=True)
    uc = p.uc
    cells = HEAP + 8 * 0x10000
    definitions = HEAP + 9 * 0x10000
    feature_entries = HEAP + 15 * 0x10000
    weapon = HEAP + 2 * 0x10000
    feature_cell = cells + (16 * GRID + 16) * 14
    put16(uc, feature_cell + 8, 0xFFFF)
    put16(uc, feature_cell + 10, 0)
    put16(uc, definitions + 0x126, 5000)
    put16(uc, definitions + 0x12C, 1)  # type 0 is replaced by type 1
    put16(uc, weapon + 0x88, 1250)

    # Native 0x4ee760 counts the +0x30/+0x2c child links before 0x4ee290
    # clones a model tree. Each definition therefore gets one valid 0x38-byte
    # leaf with no children or vertex array: structurally sufficient, but not
    # an authored 3DO feature model.
    for feature_id, root in ((0, HEAP + 0xF1000), (1, HEAP + 0xF1040)):
        uc.mem_write(root, bytes(0x38))
        put(uc, definitions + feature_id * 0x140 + 0x110, root)

    native_calls = []
    watched = {
        0x4961A0, 0x494CE0, 0x494F00, 0x496380, 0x4964B8,
        0x4EE560, 0x494A80, 0x496518, 0x495360, 0x4EE290,
        0x4EE760, 0x4EE7A0, 0x4F13A0,
    }

    def watch_native_calls(emu, address, _size, _user_data):
        if address in watched:
            native_calls.append(address)

    for address in watched:
        uc.hook_add(UC_HOOK_CODE, watch_native_calls,
                    begin=address, end=address)

    _, error = p.call(0x495360, (feature_cell, 0, 0, 0, 10))
    assert error is None, error
    assert struct.unpack("<H", uc.mem_read(feature_cell + 8, 2))[0] == 0
    assert uc.mem_read(feature_cell + 0xD, 1)[0] & 0x08
    feature_slot = struct.unpack("<H", uc.mem_read(feature_cell + 10, 2))[0]
    feature_state = feature_entries + feature_slot * 0x60
    assert read32(uc, feature_state + 4) != 0

    for damage_total in (1250, 2500, 3750):
        put(uc, shot + 0xD8, 0)
        _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
        assert error is None, error
        feature_id = struct.unpack("<H", uc.mem_read(feature_cell + 8, 2))[0]
        accumulated_damage = struct.unpack(
            "<H", uc.mem_read(feature_state + 0x26, 2))[0]
        assert feature_id == 0 and accumulated_damage == damage_total, (
            feature_id, accumulated_damage, damage_total)

    native_calls.clear()
    put(uc, shot + 0xD8, 0)
    _, error = p.call(0x529C10, (shot, 0, 1, 1, 0))
    assert error is None, error
    expected_calls = [
        0x4961A0, 0x494CE0, 0x494F00, 0x496380, 0x4964B8,
        0x4EE560, 0x494A80, 0x496518, 0x495360, 0x494A80,
        0x4EE290, 0x4EE760, 0x4EE7A0, 0x4F13A0,
    ]
    call_iter = iter(native_calls)
    assert all(any(actual == expected for actual in call_iter)
               for expected in expected_calls), [hex(address) for address in native_calls]
    replacement_id = struct.unpack("<H", uc.mem_read(feature_cell + 8, 2))[0]
    replacement_slot = struct.unpack("<H", uc.mem_read(feature_cell + 10, 2))[0]
    replacement_state = feature_entries + replacement_slot * 0x60
    replacement_damage = struct.unpack(
        "<H", uc.mem_read(replacement_state + 0x26, 2))[0]
    cell_flags = uc.mem_read(feature_cell + 0xD, 1)[0]
    assert (replacement_id, replacement_damage) == (1, 0), (
        replacement_id, replacement_damage)
    assert cell_flags & 0x08
    assert read32(uc, replacement_state + 4) != 0
    assert read32(uc, replacement_state + 0x38) == 0
    assert read32(uc, shot + 0xD8) & 2
    frees = [row for row in feature_replace if row[0] == "native-free"]
    assert frees, "native 0x4ee560 teardown did not release piece arrays"
    print("Native feature threshold replacement: type 0→1 at 5000 damage; "
          f"cell damage={replacement_damage}; flags=0x{cell_flags:02x}; "
          f"native piece free requests={len(frees)} (shimmed); projectile retired")
    print("Native feature calls: 0x4961a0→0x494ce0→0x494f00→0x496380 "
          "(0x4ee560 teardown)→0x495360 replacement→0x4ee290 clone; "
          "synthetic leaf tree and allocator/free hooks; +0x38 effect callback "
          "is null; terrain origin and renderer remain controlled")


if __name__ == "__main__":
    run()
