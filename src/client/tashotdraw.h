#pragma once

// TotalA.exe's projectile draw 0x49be60, rendertypes 1..6 (0 and 7, the
// line-drawn shots, are client/tabeamdraw.h). Drawing only: nothing here
// reaches the lockstep simulation.
//
//   1  model: the shadow, then the model's root object posed by the shot's
//      angles (+0x34, +0x36 + 0x8000, +0x38 + 0x8000; 0x49c0fa..0x49c127),
//      then, while the shot is powered (expiry +0x46 > now), the root's first
//      child (3DO +0x30: a missile's flare, a torpedo's prop) at the same
//      point; a `propeller` weapon (+0x111 bit 21) rolls that child by +0x64,
//      which gains 0x400 a tick in flight (0x49b9ae..0x49b9bc)
//      (0x49c12c..0x49c185).
//   2  a 22x22 refraction lens of strength 8 over the screen pixels at the
//      head (0x49c18f..0x49c1ed; table 0x420620 -> 0x4b91b0, blit 0x4b9360).
//   3  the shadow, then the model's root object (0x49c1f2..0x49c263; its pose
//      is uninitialised stack, docs/ta-port-plan.md "D-gun pose").
//   4  `color` >= 255 draws nothing; else the shadow, then for color 0..4 a
//      frame of an anims/fx.gaf sequence (0x49c268..0x49c37d; sequences
//      loaded at 0x429983..0x429a0b), frame (now - launch +0x42) mod count.
//   5  anims/fx.gaf "flamestream" at the head, no shadow, the frame from the
//      time left of the weapontimer (+0xe6) (0x49c382..0x49c413).
//   6  the shadow, then the model's root object posed by +0x34 as stored
//      (0x49c418..0x49c487).
//
// The shadow (1, 3, 4, 6) is fx.gaf "shadow" frame 0 (G+0x1480f, 0x429af1)
// at (x, z - ground/2), ground = the shot's +0x5e: the mean of its cell's
// highest and lowest corner heights (0x49b1ae..0x49b1c3). 0x46bae0 draws ONE
// 3DO object, ignoring its offset from its parent: children, siblings and
// offsets play no part.

#include <cstdint>
#include <optional>
#include <string_view>

namespace ta {

// Rendertype 4's sequence for `color` (0x49c30b..0x49c345); none for 5..254
// (the shadow still draws) and for 255+ (nothing draws, 0x49c270).
inline std::optional<std::string_view> taShotSequence(uint8_t color) {
    switch (color) {
        case 0: return "cannonshell";
        case 1: return "plasmasm";
        case 2: return "plasmamd";
        case 3: return "ultrashell";
        case 4: return "plasmasm";
        default: return std::nullopt;
    }
}

// Whether rendertype 4 draws the shadow (every colour below 255).
inline bool taShotDrawsShadow(uint8_t renderType, uint8_t color) {
    switch (renderType) {
        case 1: case 3: case 6: return true;
        case 4: return color < 0xff;
        default: return false;
    }
}

// Rendertype 4's frame: (now - launch) mod count (0x49c355..0x49c36a, idiv's
// remainder; the GAF frame durations play no part).
inline std::optional<uint32_t> taShotCycleFrame(int32_t elapsedTicks, uint32_t frameCount) {
    if (!frameCount) return std::nullopt;
    const int32_t r = elapsedTicks % int32_t(frameCount);
    return uint32_t(r < 0 ? r + int32_t(frameCount) : r);
}

// Rendertype 5's frame: count - (expiry - now) * count / weapontimer, drawn
// only when it falls in [0, count) (0x49c3ca..0x49c3ee).
inline std::optional<uint32_t> taShotBurnFrame(int32_t ticksLeft, uint32_t frameCount,
                                               uint16_t weaponTimerTicks) {
    if (!frameCount || !weaponTimerTicks) return std::nullopt;
    const int64_t used = int64_t(ticksLeft) * int64_t(frameCount) / int64_t(weaponTimerTicks);
    const int64_t index = int64_t(frameCount) - used;
    if (index < 0 || index >= int64_t(frameCount)) return std::nullopt;
    return uint32_t(index);
}

// The shot's +0x5e: (max + min) / 2 of the cell's four corner samples, the
// samples clipped at the map edge as the cell table's are (0x483297..0x48332c).
template<class Height>
int taShotGroundHeight(int cellX, int cellZ, int width, int height, Height&& at) {
    if (width <= 0 || height <= 0) return 0;
    int hi = 0, lo = 255;
    for (int dz = 0; dz < 2; ++dz)
        for (int dx = 0; dx < 2; ++dx) {
            const int x = cellX + dx < 0 ? 0 : cellX + dx >= width ? width - 1 : cellX + dx;
            const int z = cellZ + dz < 0 ? 0 : cellZ + dz >= height ? height - 1 : cellZ + dz;
            const int h = int(at(x, z));
            hi = h > hi ? h : hi; lo = h < lo ? h : lo;
        }
    return (hi + lo) / 2;
}

}  // namespace ta
