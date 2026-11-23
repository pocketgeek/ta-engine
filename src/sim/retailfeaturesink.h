#pragma once
// A water grave sinking (the feature update 4959c0, 495aaf..495c8d).
//
// 512ee0 gives a corpse or a statue (stone/freeze) whose ground height under
// it (511170) is at or below sea level the velocity (0, -0x2ccc, 0) (+0x14..
// +0x1c; +0x14 is not written, and is 0 here) and no decomposition timer
// (+0x3c stays 0; a land body takes the type's +0x130). A type with isfeature
// (UnitDef +0x260 & 0x1000000, the walls) gets no velocity. Every tick, after
// the units, the feature update then runs this on each record in its moving
// list (E: tools/re/check_corpse_sink.py):
//
//  - decay (+0x3c), loaded on a water grave only by a reclaim wake (406615 /
//    420ba4) while it is still on the moving list: counted down, and the
//    update that empties it starts the sink count +0x40 at 1; while +0x40
//    runs each update adds 1 and the one that takes it past 60 removes the
//    feature (496380) -- the land corpse's decomposition, which World keeps
//    in closed form (retaildeath.h retailCorpseDecayLife / Hold) since a land
//    corpse has no velocity; the body goes on moving meanwhile;
//  - fading (+0x2f): alpha (+0x30) -= step (+0x34), a float; at <= 0 the
//    feature leaves the plane (496380) and nothing else runs;
//  - no velocity, and neither +0x3c nor +0x40 running: the record moves to
//    the static list (494a80) and is never updated again (a fade not yet
//    over stops there);
//  - else position += velocity; under it the ground is 511260, the mean of
//    its cell's lowest and highest corner (-1 off the map). At or below that
//    it lands there and stops. Below sea level it sinks at -0x2ccc (x and z
//    velocity 0), and over a sea bed of 0 starts the fade, whose step
//    0x2ccc / y (5f03c8 / (y * 2^-16)) makes it last as long as the rest of
//    the fall. At or above sea level the velocity takes gravity (+0x19ecc).
//
// So a grave lies on a sea bed for good unless that bed is 0, where it fades
// out as it reaches it. Everything here is sim state (it decides when the
// body leaves the plane); the client draws the body at y.

#include <cstdint>

namespace tak::sim {

inline constexpr int32_t kRetailSinkVelocity = -0x2ccc;   // 512fdb, 495c41

struct RetailFeatureSink {
    int32_t x = 0, y = 0, z = 0;      // +0x08, +0x0c, +0x10 (16.16)
    int32_t vx = 0, vy = 0, vz = 0;   // +0x14, +0x18, +0x1c
    bool fading = false;              // +0x2f
    float alpha = 0.0f, step = 0.0f;  // +0x30, +0x34
    int32_t decay = 0, count = 0;     // +0x3c, +0x40
    bool operator==(const RetailFeatureSink&) const = default;
};

enum class RetailSinkResult : uint8_t { Moving, Settled, Removed };

// One feature-update pass over a sinking record. `ground(x, z)` is 511260.
template<class Ground>
RetailSinkResult retailFeatureSinkStep(RetailFeatureSink& f, Ground ground, uint8_t sea, int32_t gravity) {
    // 495aaf..495af1 (the model's sink into the ground, 495af6.., is display).
    if (f.decay && --f.decay == 0) f.count = 1;
    if (f.count && ++f.count > 60) return RetailSinkResult::Removed;
    if (f.fading) {
        f.alpha = f.alpha - f.step;
        if (!(f.alpha > 0.0f)) return RetailSinkResult::Removed;
    }
    // 495c92..495cad: no velocity rests the record only with no decay running.
    if (!f.vx && !f.vy && !f.vz) return f.decay || f.count ? RetailSinkResult::Moving : RetailSinkResult::Settled;
    auto add = [](int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); };
    f.x = add(f.x, f.vx); f.y = add(f.y, f.vy); f.z = add(f.z, f.vz);
    const int height = ground(f.x, f.z);
    const auto bed = int32_t(uint32_t(height) << 16);
    if (f.y <= bed) {
        f.y = bed; f.vx = f.vy = f.vz = 0;
    } else if (f.y < int32_t(uint32_t(sea) << 16)) {
        f.vx = 0; f.vy = kRetailSinkVelocity; f.vz = 0;
        if (!f.fading && height == 0) {
            // fild y; fmul 2^-16; fdivr 0x2ccc * 2^-16: y < 2^24 is exact as a
            // float, and the rounded quotient agrees with the x87's for every
            // y below 256 << 16 (checked exhaustively).
            f.fading = true; f.alpha = 1.0f; f.step = float(-kRetailSinkVelocity) / float(f.y);
        }
    } else {
        f.vy = int32_t(uint32_t(f.vy) - uint32_t(gravity));
    }
    return RetailSinkResult::Moving;
}

// A reclaim's fade (494ce0 mode 1, 494e08..494ed8) takes a NEW record from
// the free list (4949c0: +0x2e/+0x2f, +0x3c and +0x40 reset, the velocity
// left as its last user had it), copies the position and model over and frees
// the old one: the sink's own fade and any decay end, and the body stays where
// it is unless that slot still
// held a velocity (a record freed while it moved). World takes the slot's
// velocity as 0 (I: it has no model of retail's record pool).
inline void retailFeatureSinkReclaim(RetailFeatureSink& f) {
    f.vx = f.vy = f.vz = 0; f.fading = false; f.alpha = 1.0f; f.step = 0.0f; f.decay = f.count = 0;
}

// Display only: the model draw's vertex alpha for a fading record,
// ftol(alpha * 255.0f) (4ed056..4ed05f; the x87 product of a float and 255
// is exact, _ftol truncates).
inline uint8_t retailFadeByte(float alpha) {
    return alpha > 0.0f ? uint8_t(int(double(alpha) * 255.0)) : uint8_t(0);
}

// 511260: the cell under (x, z) -- the high word of each coordinate divided by
// 16 toward zero -- and the mean of its lowest and highest corner (the cell's
// +6 and +5, from 50ed60), or -1 off the map.
template<class Range>
int retailCellMidHeight(int32_t x, int32_t z, int width, int height, Range range) {
    const int cx = int(int16_t(uint32_t(x) >> 16)) / 16, cz = int(int16_t(uint32_t(z) >> 16)) / 16;
    if (cx < 0 || cz < 0 || cx >= width || cz >= height) return -1;
    const auto [low, high] = range(cx, cz);
    return (low + high) >> 1;
}

} // namespace tak::sim
