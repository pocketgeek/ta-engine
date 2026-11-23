#!/usr/bin/env python3
"""Join native ballistic sprite launch, animation clocks, and draw selection.

The default fixture uses Arapult WEAPON1's shipped cannonball art and shadow
art; `--profile aratre` covers another sprite-only weapon, and `--profile
verbal` covers a modeled Veruna bolt with a ground shadow. It reads each
sequence's real GAF/TAF frame durations and runs retail's actual
BallisticWeapon initializer, per-tick updater, animation clock, current-frame
lookup, and draw callback. Only muzzle lookup, collision, terrain height,
visibility admission, veterancy, and final renderer sinks are controlled. This
is a headless behavioral probe; it does not rasterize sprites or launch the
retail GUI.

    PYTHONPATH=tools/re python3 tools/re/probe_ballistic_sprite_timeline.py \
      build-o2/retail_visual_test
"""
import re
import struct
import subprocess
import sys
import argparse
from pathlib import Path

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("helper", nargs="?", default=str(ROOT / "build-o2/retail_visual_test"))
parser.add_argument("--profile", choices=("arapult", "aratre", "verbal"), default="arapult")
args = parser.parse_args()
HELPER = Path(args.helper)
PROFILE = {
    "arapult": {"weapon_art": "cannblg", "shadow_art": "weaponshad01", "model": None, "range": 750, "velocity": 750},
    "aratre": {"weapon_art": "cannbmed", "shadow_art": "weaponshad02", "model": None, "range": 2700, "velocity": 1150},
    "verbal": {"weapon_art": None, "shadow_art": "weaponshad01", "model": "verbal1", "range": 860, "velocity": 760},
}[args.profile]
FBI = ROOT / f"assets/extracted/all/units/{args.profile}.fbi"


def put(address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def signed32(value):
    return struct.unpack("<i", struct.pack("<I", value & 0xFFFFFFFF))[0]


def read_u16(uc, address):
    return struct.unpack("<H", uc.mem_read(address, 2))[0]


def snapshot(uc, state):
    return (read_u16(uc, state), read_u16(uc, state + 2),
            int(u32(uc, state + 8) != 0))


def whole_words(uc, address):
    raw = struct.unpack("<3i", uc.mem_read(address, 12))
    return tuple(value >> 16 for value in raw)


fbi_text = FBI.read_text(errors="replace")
weapon_match = re.search(r"\[WEAPON1\]\s*\{(.*?)\n\}", fbi_text, re.S)
assert weapon_match, f"{args.profile} WEAPON1 is missing"
weapon_text = weapon_match.group(1)
for pattern in (r"\btype\s*=\s*Ballistic\s*;",
                rf"\bshadowart\s*=\s*{PROFILE['shadow_art']}\s*;"):
    assert re.search(pattern, weapon_text, re.I), pattern
if PROFILE["weapon_art"]:
    assert re.search(rf"\bweaponart\s*=\s*{PROFILE['weapon_art']}\s*;", weapon_text, re.I)
else:
    assert not re.search(r"\bweaponart\s*=", weapon_text, re.I), weapon_text
if PROFILE["model"]:
    assert re.search(rf"\bmodel\s*=\s*{PROFILE['model']}\s*;", weapon_text, re.I)
else:
    assert not re.search(r"\bmodel\s*=", weapon_text, re.I), f"{args.profile} is expected to be sprite-only"
velocity_match = re.search(r"\bweaponvelocity\s*=\s*([0-9.]+)\s*;", weapon_text, re.I)
assert velocity_match and float(velocity_match.group(1)) == PROFILE["velocity"], weapon_text

p = Icd()
uc = p.uc
uc.reg_write(UC_X86_REG_FPCW, 0x027F)
game, config, settings, shot, weapon, wrapper, source, unit_type, unit_art = [
    HEAP + i * 0x18000 for i in range(1, 10)]
foreground_anim, shadow_anim = HEAP + 0x1A0000, HEAP + 0x1B0000
explored = HEAP + 0x1C0000
foreground_state, shadow_state = shot + 0x3C, shot + 0x48

def read_sequence(bank, name):
    path = ROOT / "assets/extracted/all/anims" / bank
    raw = path.read_bytes()
    version, count = struct.unpack_from("<II", raw, 0)
    assert version == 0x00010100, (path, hex(version))
    for index in range(count):
        entry = struct.unpack_from("<I", raw, 12 + index * 4)[0]
        frames = struct.unpack_from("<H", raw, entry)[0]
        loop = raw[entry + 2]
        actual_name = raw[entry + 8:entry + 40].split(b"\0", 1)[0].decode("ascii")
        if actual_name.casefold() == name.casefold():
            durations = [struct.unpack_from("<I", raw, entry + 44 + frame * 8)[0] & 0xFFFF
                         for frame in range(frames)]
            return loop, durations
    raise AssertionError(f"{name!r} was not found in {path}")


if PROFILE["weapon_art"]:
    foreground_loop, foreground_durations = read_sequence(
        f"{PROFILE['weapon_art']}_4444.taf", PROFILE["weapon_art"])
else:
    foreground_loop, foreground_durations = 0, []
shadow_loop, shadow_durations = read_sequence("shadows.gaf", PROFILE["shadow_art"])
assert shadow_durations and (foreground_durations or PROFILE["model"])
animation_frames = {}


def make_animation(address, durations, loop=True):
    count = len(durations)
    metadata = bytearray(0x30 + count * 8)
    struct.pack_into("<HB", metadata, 0, count, int(loop))
    frame_ptrs = []
    for index, duration in enumerate(durations):
        frame_ptr = HEAP + 0x1D0000 + len(animation_frames) * 0x100 + index * 4
        frame_ptrs.append(frame_ptr)
        struct.pack_into("<I", metadata, 0x28 + index * 8, frame_ptr)
        struct.pack_into("<H", metadata, 0x2C + index * 8, duration)
        animation_frames[frame_ptr] = index
    uc.mem_write(address, bytes(metadata))
    return frame_ptrs


foreground_frames = (make_animation(foreground_anim, foreground_durations, foreground_loop)
                     if foreground_durations else [])
shadow_frames = make_animation(shadow_anim, shadow_durations, shadow_loop)
events = []
tick = 0
origin = (1280 * 65536, 64 * 65536, 1280 * 65536)
camera = (1024, 960)
terrain_height = 88
sprite_rows = []
collision_substeps = 0
projectile_model = HEAP + 0x1E0000


def muzzle(uc, sp):
    args = struct.unpack("<4I", uc.mem_read(sp, 16))
    assert args == (source, shot + 4, 0, 0xFFFFFFFF), args
    uc.mem_write(shot + 4, struct.pack("<3i", *origin))
    return 4, 0


def collision(uc, sp):
    global collision_substeps
    assert struct.unpack("<I", uc.mem_read(sp, 4))[0] == shot
    collision_substeps += 1
    return 1, 0


def terrain(uc, sp):
    assert struct.unpack("<I", uc.mem_read(sp, 4))[0] == shot + 4
    return 1, terrain_height


def viewport(_uc, _sp):
    return 1, 1


def sprite(uc, sp):
    values = struct.unpack("<6I", uc.mem_read(sp, 24))
    signed = (values[0], signed32(values[1]), signed32(values[2]), *values[3:])
    events.append(("sprite", signed))
    return 6, 0


def draw_model(uc, sp):
    args = struct.unpack("<4I", uc.mem_read(sp, 16))
    events.append(("model", args))
    return 4, 0


def veteran(_uc, _sp):
    return 1, 0


p.hooks.update({
    0x4DD420: muzzle,
    0x48C870: viewport,
    0x4FAC00: sprite,
    0x4FF570: draw_model,
    0x511170: terrain,
    0x52A4D0: collision,
    0x519310: veteran,
    0x530730: lambda _uc, _sp: (1, 0),
})
p.freeze_hooks()

# Native drawing reads the selected player's explored plane and a live viewport.
put(0x62D55C, game)
put(0x62D558, config)
put(config + 8, settings)
uc.mem_write(settings + 0x15, b"\x01")
uc.mem_write(game + 0x306F, b"\x00")
player = game + 0x2404
put(player + 0x88, explored)
put(player + 0x8C, 128)
put(player + 0x90, 128)
uc.mem_write(explored, bytes([1]) * (128 * 128))
put(game + 0x14ED0, camera[0])
put(game + 0x14ED4, camera[1])
put(game + 0x19F44, 100)
put(game + 0x19ECC, 8155)

# The selected unit's actual authored ballistic profile and animation sequences.
put(source + 0xB4, unit_type)
put(unit_type + 0x8A, unit_art)
put(wrapper, weapon)
put(weapon + 0x48, projectile_model if PROFILE["model"] else 0)
put(weapon + 0x4C, 0)
put(weapon + 0x50, 10)
put(weapon + 0x54, foreground_anim if foreground_durations else 0)
put(weapon + 0x58, shadow_anim)
put(weapon + 0x90, PROFILE["range"])
put(weapon + 0xCC, (PROFILE["velocity"] * 65536 // 30) // 2)
put(weapon + 0xD0, 2)
uc.mem_write(source + 0x7E, struct.pack("<H", 0x1234))
uc.mem_write(wrapper + 0x16, struct.pack("<H", 0x4000))
uc.mem_write(wrapper + 0x18, struct.pack("<H", 0x0800))
uc.mem_write(wrapper + 0x1A, struct.pack("<H", 0))
uc.mem_write(weapon + 0xB8, bytes(6))
uc.mem_write(weapon + 0xD4, struct.pack("<f", 1.0))
uc.mem_write(shot, bytes(0xDC))
put(shot, weapon)

_, error = p.call(0x52BE80, (shot, source, wrapper))
assert error is None, error
if PROFILE["model"]:
    assert u32(uc, shot + 0x93) == projectile_model, (u32(uc, shot + 0x93), projectile_model)
else:
    assert u32(uc, shot + 0x93) == 0, "sprite-only shot unexpectedly selected a model"
if foreground_durations:
    assert u32(uc, foreground_state + 8) == foreground_anim
assert u32(uc, shadow_state + 8) == shadow_anim
clock_traces = []
if foreground_durations:
    clock_traces.append((foreground_loop, foreground_durations,
                         [*snapshot(uc, foreground_state)], foreground_state))
clock_traces.append((shadow_loop, shadow_durations, [*snapshot(uc, shadow_state)], shadow_state))

steps = 12
for age in range(steps + 1):
    tick = 101 + age
    put(game + 0x19F44, tick)
    events.clear()
    _, error = p.call(0x52C100, (shot,), ecx=weapon)
    assert error is None, ("native draw", age, error)

    fg_frame, fg_remaining, fg_active = snapshot(uc, foreground_state)
    sh_frame, sh_remaining, sh_active = snapshot(uc, shadow_state)
    assert sh_active, (age, sh_frame)
    sh_ptr = shadow_frames[sh_frame]
    # The native ballistic draw submits ground shadow before foreground art.
    x, y, z = whole_words(uc, shot + 4)
    expected_shadow = (sh_ptr, x - camera[0], z - camera[1] - (terrain_height >> 1), 1, 0, 0)
    expected = [("sprite", expected_shadow)]
    if PROFILE["model"]:
        expected.append(("model", (shot + 4, projectile_model, shot + 0x34, u32(uc, shot + 0x54))))
    if foreground_durations:
        assert fg_active, (age, fg_frame)
        fg_ptr = foreground_frames[fg_frame]
        expected_foreground = (fg_ptr, x - camera[0], z - camera[1] - (y >> 1), 0, 0, 0)
        expected.append(("sprite", expected_foreground))
    assert events == expected, (age, events, expected, fg_frame, sh_frame)
    # Record the exact pair of selected frame pointers and callback coordinates.
    sprite_rows.append((age, *[event[1] for event in events]))
    if age < steps:
        _, error = p.call(0x52BF90, (shot,), ecx=weapon)
        assert error is None, ("native ballistic update", age, error)
        for _, _, trace, state in clock_traces:
            trace.extend(snapshot(uc, state))

assert len(sprite_rows) == steps + 1
assert collision_substeps == steps * 2, collision_substeps

# Join each launch/update clock with the shared World clock implementation.
clock_inputs = []
for loop, durations, _, _ in clock_traces:
    clock_inputs.append(" ".join(map(str, [len(durations), loop, 0, steps, *durations])))
result = subprocess.run([str(HELPER), "--effect-clock"], input="\n".join(clock_inputs) + "\n",
                        text=True, capture_output=True, check=True)
ported_clocks = [list(map(int, line.split())) for line in result.stdout.splitlines()]
assert len(ported_clocks) == len(clock_traces), result.stdout
for index, (actual, (_, _, native, _)) in enumerate(zip(ported_clocks, clock_traces)):
    assert actual == native, (index, actual, native)

assert len(sprite_rows) == steps + 1
draws = ", ".join("shadow + " + ("model" if PROFILE["model"] else "foreground art")
                   for _ in range(1))
print(f"PASS: {args.profile}'s authored BallisticWeapon runs native launch, "
      f"{steps} updates/{collision_substeps} admitted substeps, "
      f"{len(clock_traces)} authored animation clock(s), current-frame lookup, "
      f"and {steps + 1} exact {draws} draw selections; World clocks match via {HELPER}")
