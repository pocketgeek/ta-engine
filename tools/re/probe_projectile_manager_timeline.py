#!/usr/bin/env python3
"""Exercise one live Araarch-speed ballistic shell through retail's pool manager.

This is a headless Unicorn trace: it does not start the retail UI. The path calls
the native manager initializer, slot allocator, aim/launch routines, and per-tick
projectile manager, then compares each resulting ballistic state with the native
retail_visual_test stepping helper.

    PYTHONPATH=tools/re python3 tools/re/probe_projectile_manager_timeline.py
"""
import argparse
import struct
import subprocess
from pathlib import Path

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HELPER = ROOT / "build-o2/retail_visual_test"
PROJECTILE_UPDATE = 0x52BF90
PROJECTILE_COMPACT = 0x52A800
MAP_COLLISION = 0x52A4D0


def read_u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def write_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def read_i32s(uc, address, count=3):
    return struct.unpack("<" + "i" * count, uc.mem_read(address, count * 4))


def retail_step(helper, state, gravity=8155, adjustment=1.0, substeps=2):
    payload = "T " + " ".join(str(value) for value in (
        *state[0], *state[1], *state[2], gravity, adjustment, substeps, 0, 0, 0,
    )) + "\n"
    result = subprocess.run(
        [str(helper), "--ballistic-projectile"], input=payload,
        text=True, capture_output=True, check=True,
    )
    values = tuple(int(value) for value in result.stdout.split())
    if len(values) != 9:
        raise RuntimeError(f"unexpected ballistic helper output: {result.stdout!r}")
    return values[:3], values[3:6], values[6:9]


def make_emulator():
    p = Icd()
    uc = p.uc
    game = HEAP + 0x10000
    pool = HEAP + 0x30000
    weapon = HEAP + 0x50000
    weapon_iface = HEAP + 0x60000
    source = HEAP + 0x70000
    target = HEAP + 0x70200
    wrapper = HEAP + 0x71000
    aim_context = HEAP + 0x72000
    aim_delta = HEAP + 0x73000
    cells = HEAP + 0x80000
    feature_table = HEAP + 0x90000
    feature_links = HEAP + 0x110000
    units = HEAP + 0xA0000
    unit_kind = HEAP + 0xB0000
    model = HEAP + 0xC0000
    vertices = HEAP + 0xD0000
    primitive = HEAP + 0xE0000
    indices = HEAP + 0xF0000
    rules = HEAP + 0x100000

    allocations = []
    impacts = []

    # This is the allocator edge used by the real projectile-manager initializer.
    def allocate_array(uc, stack):
        _name, size = struct.unpack("<II", uc.mem_read(stack, 8))
        allocations.append(size)
        return 0, pool  # cdecl: caller removes arguments

    # Native impact dispatch reaches retail side effects outside this isolated
    # fixture. Record the real impact call and write the same retirement bit that
    # those side effects set before manager compaction.
    def impact_sink(uc, stack):
        shot = struct.unpack("<I", uc.mem_read(stack, 4))[0]
        args = struct.unpack("<5I", uc.mem_read(stack, 20))
        tick = read_u32(uc, game + 0x19F44)
        position = tuple(round(value / 65536, 3) for value in read_i32s(uc, shot + 4))
        impacts.append((tick, args, position))
        write_u32(uc, shot + 0xD8, read_u32(uc, shot + 0xD8) | 2)
        return 5, 0

    # Retail's ballistic update calls this native muzzle-query bridge. Return the
    # same Araarch muzzle used by the World fixture below.
    def query_muzzle(uc, stack):
        args = struct.unpack("<4I", uc.mem_read(stack, 16))
        out = args[1]
        uc.mem_write(out, struct.pack("<3i", 225 * 65536, 20 * 65536, 400 * 65536))
        return 4, 0

    p.hooks.update({
        0x529C10: impact_sink,
        0x5BA3D0: allocate_array,
        0x4DD420: query_muzzle,
    })
    p.freeze_hooks()
    uc.reg_write(UC_X86_REG_FPCW, 0x027F)

    write_u32(uc, 0x62D55C, game)
    write_u32(uc, game + 0x19E98, 64)
    write_u32(uc, game + 0x19E9C, 64)
    write_u32(uc, game + 0x19E74, feature_links)
    write_u32(uc, game + 0x19F04, cells)
    write_u32(uc, game + 0x19ECC, 8155)
    write_u32(uc, game + 0x19EC0, 1)
    write_u32(uc, game + 0x19EDC, feature_table)
    write_u32(uc, game + 0x175DC, rules)
    write_u32(uc, rules + 0xD3D, 0)
    uc.mem_write(game + 0x19EF8, b"\0")
    write_u32(uc, game + 0x14E84, units)
    write_u32(uc, game + 0x14E88, units + 0x138)
    write_u32(uc, game + 0x19F44, 0)

    # Flat 64x64 terrain with one height-255 feature at (28,25), world (448,400).
    for z in range(64):
        for x in range(64):
            cell = cells + (z * 64 + x) * 14
            uc.mem_write(cell + 5, bytes((10, 10)))
            uc.mem_write(cell + 8, b"\xff\xff")
    tree_cell = cells + (25 * 64 + 28) * 14
    uc.mem_write(tree_cell + 8, b"\0\0")
    uc.mem_write(feature_table + 0x138, b"\xff")

    # One selected enemy occupies the real unit-table slot. Its simple quad is
    # only needed for the later unit-collision query; the earlier tree must win.
    kind = unit_kind
    write_u32(uc, target + 0xB4, kind)
    write_u32(uc, kind + 0x2A0, model)
    write_u32(uc, model + 0x0C, 0)
    write_u32(uc, model + 0x24, vertices)
    write_u32(uc, model + 0x28, primitive)
    write_u32(uc, primitive + 0x0C, indices)
    uc.mem_write(indices, struct.pack("<4H", 0, 1, 2, 3))
    quad = ((-20, -20), (20, -20), (20, 20), (-20, 20))
    uc.mem_write(vertices, b"".join(
        struct.pack("<3i", x * 65536, 0, z * 65536) for x, z in quad
    ))
    write_u32(uc, kind + 0x14A, 80 * 65536)
    write_u32(uc, target + 0x68, 520 * 65536)
    write_u32(uc, target + 0x6C, 20 * 65536)
    write_u32(uc, target + 0x70, 400 * 65536)
    write_u32(uc, target + 0x130, 0x01000000)
    uc.mem_write(target + 0xFD, b"\x01")
    for z in range(24, 27):
        for x in range(31, 34):
            uc.mem_write(cells + (z * 64 + x) * 14, struct.pack("<H", 1))

    # Araarch-speed BallisticWeapon interface and source unit.
    write_u32(uc, wrapper, weapon)
    write_u32(uc, weapon + 0x40, weapon_iface)
    write_u32(uc, weapon_iface, 0x5F36DC)  # update slot +4 is 0x52bf90
    uc.mem_write(weapon_iface + 8, struct.pack("<f", 1.0))
    write_u32(uc, weapon + 0x90, 450)
    write_u32(uc, weapon + 0xCC, (750 * 65536 // 30) // 2)
    write_u32(uc, weapon + 0xD0, 2)
    write_u32(uc, weapon + 0xC8, 0)
    uc.mem_write(weapon + 0xB8, bytes(6))
    uc.mem_write(weapon + 0xD4, struct.pack("<f", 1.0))
    write_u32(uc, source + 0xB4, kind)
    uc.mem_write(source + 0x7E, b"\0\0")

    # Ask the actual retail aim and launch routines for the target-relative
    # delta from muzzle x=225 to target x=520.
    write_u32(uc, aim_context + 4, 0)
    uc.mem_write(aim_context + 8, struct.pack("<f", 1.0))
    uc.mem_write(aim_delta, struct.pack("<3i", 295 * 65536, 0, 0))
    _eax, error = p.call(0x52BDF0, (source, wrapper, aim_delta), ecx=aim_context)
    if error:
        raise RuntimeError(f"native aim failed: {error}")

    _eax, error = p.call(0x52AA20)
    if error:
        raise RuntimeError(f"native projectile manager init failed: {error}")
    if read_u32(uc, 0x641204) != 0 or read_u32(uc, 0x641208) != pool:
        raise RuntimeError("native projectile manager did not initialize the expected pool")
    if allocations != [0x101D0]:
        raise RuntimeError(f"unexpected projectile pool allocation: {allocations}")

    _eax, error = p.call(0x529A90)
    shot = read_u32(uc, 0x641208)
    if error or shot != pool or read_u32(uc, 0x641204) != 1:
        raise RuntimeError(f"native projectile allocation failed: {error}")
    _eax, error = p.call(0x52BE80, (shot, source, wrapper))
    if error:
        raise RuntimeError(f"native ballistic launch failed: {error}")
    # These are the ordinary caller-owned fields surrounding the launch
    # initializer; keep one active, unexpired object in player 1's pool.
    write_u32(uc, shot, weapon)
    write_u32(uc, shot + 0x7C, source)
    write_u32(uc, shot + 0x60, 1)
    write_u32(uc, shot + 0x64, 0)
    write_u32(uc, shot + 0x70, 1000)

    expected = (
        read_i32s(uc, shot + 4),
        read_i32s(uc, shot + 0x1C),
        struct.unpack("<3H", uc.mem_read(shot + 0x34, 6)),
    )
    expected_start = (
        (225 * 65536, 20 * 65536, 400 * 65536),
        (819000, 20100, 0),
        (0, 16384, 306),
    )
    if expected != expected_start:
        raise RuntimeError(f"unexpected native launch state: {expected!r}")

    manager_calls = []

    def trace_native_manager(uc, address, _size, _user):
        if address in (PROJECTILE_UPDATE, MAP_COLLISION, PROJECTILE_COMPACT):
            manager_calls.append((read_u32(uc, game + 0x19F44), address))

    uc.hook_add(UC_HOOK_CODE, trace_native_manager)
    return p, game, shot, expected, impacts, manager_calls


def run(helper):
    p, game, shot, expected, impacts, manager_calls = make_emulator()
    uc = p.uc
    print("native launch:", expected)
    states = []
    for tick in range(1, 10):
        write_u32(uc, game + 0x19F44, tick)
        _eax, error = p.call(0x52AFD0)
        if error:
            raise RuntimeError(f"native manager failed on tick {tick}: {error}")
        actual = (
            read_i32s(uc, shot + 4),
            read_i32s(uc, shot + 0x1C),
            struct.unpack("<3H", uc.mem_read(shot + 0x34, 6)),
        )
        expected = retail_step(helper, expected)
        if actual != expected:
            raise RuntimeError(f"tick {tick}: native pool state {actual!r} != helper {expected!r}")
        count = read_u32(uc, 0x641204)
        calls_this_tick = [address for at, address in manager_calls if at == tick]
        if calls_this_tick.count(PROJECTILE_UPDATE) != 1:
            raise RuntimeError(f"tick {tick}: expected one native projectile update, got {calls_this_tick!r}")
        if calls_this_tick.count(MAP_COLLISION) < 1:
            raise RuntimeError(f"tick {tick}: native ballistic update did not query map collision")
        if calls_this_tick.count(PROJECTILE_COMPACT) != 1:
            raise RuntimeError(f"tick {tick}: expected one native pool compaction, got {calls_this_tick!r}")
        if tick < 9 and (count != 1 or impacts):
            raise RuntimeError(f"projectile retired early on tick {tick}; count={count}, impacts={impacts!r}")
        if tick == 9 and (count != 0 or len(impacts) != 1):
            raise RuntimeError(f"expected impact and same-tick retirement on tick 9; count={count}, impacts={impacts!r}")
        states.append(actual)

    impact_tick, impact_args, impact_position = impacts[0]
    if impact_tick != 9 or impact_args[1:] != (0, 1, 1, 0):
        raise RuntimeError(f"unexpected native tree-impact callback: {impacts!r}")
    print("native manager: one update, map query, and compaction per tick; active through ticks 1–8")
    print(f"native impact/retirement: tick {impact_tick}, position {impact_position}, pool count 0")
    print("native per-tick XYZ state matches retail_ballistic_projectile helper through tick 9")
    print("LIMIT: impact callback is intercepted after real collision; native damage/FX internals are outside this fixture")
    return states


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("helper", nargs="?", type=Path, default=DEFAULT_HELPER,
                        help="retail_visual_test binary (default: %(default)s)")
    args = parser.parse_args()
    run(args.helper)


if __name__ == "__main__":
    main()
