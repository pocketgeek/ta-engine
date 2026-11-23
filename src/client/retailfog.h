#pragma once

#include "sim/retailexploration.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// The local display's fog of war, as retail shows it (KINGDOMS.icd; see
// docs/retail-engine.md, "Fog of war display"). Retail keeps ONE sight plane per
// player -- the view counts at player+0x88, stamped by 0x4c6800 on 32 px cells
// of the projected map (x>>5, (z - y/2)>>5) -- and both the game logic and the
// display read it. The simulation already maintains that plane for every player
// (World::viewCounts, hashed); this header only turns it into what the local
// screen shows: the fog overlay, which other players' units the main view draws
// and lets the cursor pick, and the minimap's dots. Display only: nothing here
// is read by the simulation.
namespace tak::retailfog {

// One 32 px projected cell of the local display plane.
enum : uint8_t { kUnexplored = 0, kExplored = 1, kInView = 2 };

// What the local display plane is made of. `viewers` are the players whose
// current view the local screen shows (World::viewShareMask of the local
// player: it and every ally but a passive neutral); `player` is the local
// player, whose Mapping (explored) bit is read.
struct PlaneSource {
    std::span<const uint16_t> viewCounts;   // player-major, width*height per player
    std::span<const uint16_t> explored;     // Mapping bits, one per player
    int width = 0, height = 0;
    uint32_t viewers = 0;
    int player = -1;
    bool mapRevealed = false;   // Map Revealed / +Mapping: every cell explored
    bool losOff = false;        // +LOS Off / +NowISee: explored cells are in view
};

// The display plane: unexplored where the Mapping bit is clear, in view where
// any viewer's count is nonzero, otherwise explored. With a rectangle (cells
// [x0,x1) x [z0,z1)) only that part is rebuilt -- a plane of the wrong size is
// always rebuilt whole. Returns whether `out` changed (so the overlay texture is
// re-uploaded only then).
inline bool buildPlane(const PlaneSource& s, std::vector<uint8_t>& out,
                       int x0 = 0, int z0 = 0, int x1 = -1, int z1 = -1) {
    const int width = std::max(0, s.width), height = std::max(0, s.height);
    const size_t cells = size_t(width) * size_t(height);
    std::array<const uint16_t*, 32> planes{};
    size_t count = 0;
    for (int p = 0; p < 32; ++p)
        if ((s.viewers >> p & 1) && (size_t(p) + 1) * cells <= s.viewCounts.size())
            planes[count++] = s.viewCounts.data() + size_t(p) * cells;
    const bool haveExplored = s.explored.size() == cells && s.player >= 0 && s.player < 16;
    const uint16_t bit = haveExplored ? uint16_t(1u << s.player) : 0;
    bool changed = false;
    if (out.size() != cells || x1 < 0) {
        changed = out.size() != cells;
        out.resize(cells);
        x0 = z0 = 0; x1 = width; z1 = height;
    }
    x0 = std::clamp(x0, 0, width); x1 = std::clamp(x1, x0, width);
    z0 = std::clamp(z0, 0, height); z1 = std::clamp(z1, z0, height);
    uint8_t* o = out.data();
    // 0x498270 reads the Mapping bit first: a cell never explored is black
    // whatever its view count says. A large map has a million cells, so four
    // cells at a time in 16-bit lanes.
    constexpr uint64_t kLow = 0x7fff7fff7fff7fffull, kHigh = 0x8000800080008000ull;
    const auto nonzero = [](uint64_t v) { return (((v & kLow) + kLow) | v) & kHigh; };   // lane != 0
    const uint64_t lanes = uint64_t(bit) * 0x0001000100010001ull;
    for (int z = z0; z < z1; ++z) {
        size_t i = size_t(z) * size_t(width) + size_t(x0);
        const size_t end = size_t(z) * size_t(width) + size_t(x1);
        for (; i + 4 <= end; i += 4) {
            uint64_t explored = 0;
            if (s.mapRevealed) explored = kHigh;
            else if (haveExplored) { uint64_t m; std::memcpy(&m, s.explored.data() + i, 8); explored = nonzero(m & lanes); }
            const uint64_t e = explored >> 15;   // 1 in each explored lane
            uint64_t v = e << 1;                 // LOS off: explored is in view
            if (!s.losOff) {
                uint64_t seen = 0;
                for (size_t k = 0; k < count; ++k) { uint64_t c; std::memcpy(&c, planes[k] + i, 8); seen |= c; }
                v = e + (e & (nonzero(seen) >> 15));
            }
            uint64_t x = (v | (v >> 8)) & 0x0000ffff0000ffffull;   // the four lanes' low bytes
            const uint32_t packed = uint32_t((x | (x >> 16)) & 0xffffffffull);
            uint32_t before; std::memcpy(&before, o + i, 4);
            changed = changed || before != packed;
            std::memcpy(o + i, &packed, 4);
        }
        for (; i < end; ++i) {
            uint8_t v = kUnexplored;
            if (s.mapRevealed || (haveExplored && (s.explored[i] & bit))) {
                v = s.losOff ? kInView : kExplored;
                for (size_t k = 0; k < count && v != kInView; ++k)
                    if (planes[k][i]) v = kInView;
            }
            changed = changed || o[i] != v;
            o[i] = v;
        }
    }
    return changed;
}

inline bool cellInView(std::span<const uint8_t> plane, int w, int h, int cx, int cz) {
    return uint32_t(cx) < uint32_t(w) && uint32_t(cz) < uint32_t(h) &&
           plane[size_t(cz) * size_t(w) + size_t(cx)] == kInView;
}

// 421d00 for a 16.16 world point: its projected whole-pixel cell.
inline bool pointInView(std::span<const uint8_t> plane, int w, int h, int32_t x, int32_t y, int32_t z) {
    const int px = int16_t(uint32_t(x) >> 16), py = int16_t(uint32_t(y) >> 16), pz = int16_t(uint32_t(z) >> 16);
    return cellInView(plane, w, h, px >> 5, (pz - (py >> 1)) >> 5);
}

// 4f7210 (LosOn) on the display plane for a unit the local side does not own:
// never while it is cloaked or carried, otherwise when any of the four
// projected bounding-box corners (+/-footX*8, +/-footZ*8; top y+modelTop for the
// far pair, bottom y for the near pair) lies on a cell in view. The same corner
// rule as World::playerSees, on the merged local plane.
inline bool unitInView(std::span<const uint8_t> plane, int w, int h, bool hidden,
                       int32_t x, int32_t y, int32_t z, int footX, int footZ, int32_t modelTop) {
    if (hidden) return false;
    const int32_t hx = int32_t(uint32_t(8 * footX) << 16), hz = int32_t(uint32_t(8 * footZ) << 16);
    const int32_t x0 = int32_t(uint32_t(x) - uint32_t(hx)), x1 = int32_t(uint32_t(x) + uint32_t(hx));
    const int32_t z0 = int32_t(uint32_t(z) - uint32_t(hz)), z1 = int32_t(uint32_t(z) + uint32_t(hz));
    const int32_t top = int32_t(uint32_t(y) + uint32_t(modelTop));
    return pointInView(plane, w, h, x0, top, z0) || pointInView(plane, w, h, x1, top, z0) ||
           pointInView(plane, w, h, x1, y, z1) || pointInView(plane, w, h, x0, y, z1);
}

// The minimap's radar (0x4f9970): every unit with a radar distance that the
// local player owns (built, not status-locked) marks each other player's
// uncloaked unit within that distance (0x509ac0's circle on 16.16 positions,
// the high words of the squared terms; callback 0x4f98e0). Radar never clears
// fog and never shows a unit in the main view -- only its minimap dot.
class RadarCover {
public:
    void clear() { sources_.clear(); head_.clear(); next_.clear(); }
    void add(int32_t x, int32_t z, int32_t radius) {
        if (radius > 0) sources_.push_back({x, z, int64_t(radius) * radius});
    }
    // Buckets the sources; call after the last add. A bucket is small enough that
    // a source in the query's own bucket always reaches it, so a unit inside the
    // radar (the common case in a battle) is answered from its first bucket.
    void finish() {
        head_.clear(); next_.clear();
        if (sources_.empty()) return;
        int64_t minR2 = INT64_MAX; maxR2_ = 0;
        for (const auto& s : sources_) { minR2 = std::min(minR2, s.r2); maxR2_ = std::max(maxR2_, s.r2); }
        cell_ = std::max(64, int(std::sqrt(double(minR2)) / 1.5));
        range_ = int(std::ceil(std::sqrt(double(maxR2_)) / cell_));
        int minX = INT32_MAX, minZ = INT32_MAX, maxX = INT32_MIN, maxZ = INT32_MIN;
        for (const auto& s : sources_) {
            const int bx = floorDiv(s.x >> 16), bz = floorDiv(s.z >> 16);
            minX = std::min(minX, bx); maxX = std::max(maxX, bx);
            minZ = std::min(minZ, bz); maxZ = std::max(maxZ, bz);
        }
        x0_ = minX; z0_ = minZ; bw_ = maxX - minX + 1; bh_ = maxZ - minZ + 1;
        head_.assign(size_t(bw_) * size_t(bh_), -1);
        next_.assign(sources_.size(), -1);
        for (size_t i = 0; i < sources_.size(); ++i) {
            const size_t b = size_t(floorDiv(sources_[i].z >> 16) - z0_) * size_t(bw_) +
                             size_t(floorDiv(sources_[i].x >> 16) - x0_);
            next_[i] = head_[b]; head_[b] = int(i);
        }
    }
    bool empty() const { return sources_.empty(); }
    bool covers(int32_t x, int32_t z) const {
        if (head_.empty()) return false;
        const int px = x >> 16, pz = z >> 16;
        const int bx = floorDiv(px) - x0_, bz = floorDiv(pz) - z0_;
        const auto bucket = [&](int cx, int cz) {
            if (cx < 0 || cz < 0 || cx >= bw_ || cz >= bh_) return false;
            // Skip a bucket whose nearest point is beyond the largest radius.
            const int64_t left = int64_t(cx + x0_) * cell_, top = int64_t(cz + z0_) * cell_;
            const int64_t nx = std::clamp<int64_t>(px, left, left + cell_) - px;
            const int64_t nz = std::clamp<int64_t>(pz, top, top + cell_) - pz;
            if (nx * nx + nz * nz > maxR2_ + 2 * cell_) return false;
            for (int i = head_[size_t(cz) * size_t(bw_) + size_t(cx)]; i >= 0; i = next_[size_t(i)]) {
                const auto& s = sources_[size_t(i)];
                const int64_t ddx = int64_t(x) - s.x, ddz = int64_t(z) - s.z;
                if (((ddx * ddx) >> 32) + ((ddz * ddz) >> 32) <= s.r2) return true;
            }
            return false;
        };
        if (bucket(bx, bz)) return true;
        for (int k = 1; k <= range_; ++k)   // rings outward
            for (int d = -k; d <= k; ++d)
                if (bucket(bx + d, bz - k) || bucket(bx + d, bz + k) ||
                    (d > -k && d < k && (bucket(bx - k, bz + d) || bucket(bx + k, bz + d))))
                    return true;
        return false;
    }
private:
    int floorDiv(int v) const { return v >= 0 ? v / cell_ : -((-v + cell_ - 1) / cell_); }
    struct Source { int32_t x, z; int64_t r2; };
    std::vector<Source> sources_;
    std::vector<int> head_, next_;
    int64_t maxR2_ = 0;
    int cell_ = 64, range_ = 1, x0_ = 0, z0_ = 0, bw_ = 0, bh_ = 0;
};

// ---- the overlay (Glide path, 0x498270) ------------------------------------
// One byte per cell, alpha in the high nibble and intensity in the low one:
// unexplored 0xf0 (opaque black), explored but out of view 0x8f (fog colour
// at 8/15), in view 0x0f (clear). With LOS off retail writes 0x0f for every
// explored cell; buildPlane has already made those cells kInView.
constexpr uint8_t cellByte(uint8_t v) { return v == kInView ? 0x0f : v == kExplored ? 0x8f : 0xf0; }

// 0x498b60: one ARGB4444 texel of the 2x upsampled overlay, from its own cell
// (weight 19/32), the two edge neighbours toward that corner (5/32 each) and the
// diagonal one (3/32), each nibble separately, rounded down.
constexpr uint8_t blendTexel(uint8_t own, uint8_t side1, uint8_t side2, uint8_t diagonal) {
    const int hi = ((own >> 4) * 19 + ((side1 >> 4) + (side2 >> 4)) * 5 + (diagonal >> 4) * 3) >> 5;
    const int lo = ((own & 15) * 19 + ((side1 & 15) + (side2 & 15)) * 5 + (diagonal & 15) * 3) >> 5;
    return uint8_t(hi << 4 | lo);
}

// Writes the 2x2 texels of cells [x0,x1) x [z0,z1) as RGBA bytes (R=G=B the
// intensity, A the alpha, both nibbles scaled to 0..255) into `pixels`, whose
// rows are `pitch` bytes and whose origin is texel (2*x0, 2*z0). Neighbours
// past the map edge repeat the edge cell (retail copies its edge texels).
inline void writeTexels(std::span<const uint8_t> plane, int w, int h, int x0, int z0, int x1, int z1,
                        uint8_t* pixels, int pitch) {
    auto at = [&](int x, int z) {
        x = std::clamp(x, 0, w - 1); z = std::clamp(z, 0, h - 1);
        return cellByte(plane[size_t(z) * size_t(w) + size_t(x)]);
    };
    auto put = [&](int tx, int tz, uint8_t v) {
        uint8_t* p = pixels + size_t(tz) * size_t(pitch) + size_t(tx) * 4;
        const uint8_t i = uint8_t((v & 15) * 17), a = uint8_t((v >> 4) * 17);
        p[0] = p[1] = p[2] = i; p[3] = a;
    };
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x) {
            const uint8_t c = at(x, z), l = at(x - 1, z), r = at(x + 1, z), u = at(x, z - 1), d = at(x, z + 1);
            const int tx = (x - x0) * 2, tz = (z - z0) * 2;
            put(tx, tz, blendTexel(c, l, u, at(x - 1, z - 1)));
            put(tx + 1, tz, blendTexel(c, r, u, at(x + 1, z - 1)));
            put(tx, tz + 1, blendTexel(c, l, d, at(x - 1, z + 1)));
            put(tx + 1, tz + 1, blendTexel(c, r, d, at(x + 1, z + 1)));
        }
}

// RollingFog and WavyFog (both on by default; console toggles at 0x425ef0 /
// 0x425f10, constants set by 0x497900). `t` is real time in seconds (0x5ba760,
// QueryPerformanceCounter); X and P are the vertex's projected map position in
// pixels (retail: screen position + camera). The overlay is drawn as a grid of
// quads whose vertices carry the colour (fog colour times brightness) and,
// inside the grid, are displaced by the wave.
struct Wave {
    double t = 0;
    float rollingPhase = 0, wavyPhase = 0;
    explicit Wave(double seconds) : t(seconds),
        rollingPhase(float(std::sin(0.5f * seconds))),
        wavyPhase(float(std::sin((seconds + 1.2) * 0.5f))) {}
    // sin(P/32*0.31 + 0.17t) * sin(X/32*0.11 - 0.4t), the shared spatial term.
    float field(float X, float P) const {
        return float(std::sin(double(P) * 0.03125 * 0.31f + 0.17f * t) *
                     std::sin(double(X) * 0.03125 * 0.11f + -0.4f * t));
    }
    // 0x498c70: (field * sin(0.5t) + 1) / 2, multiplied into the fog colour.
    float brightness(float X, float P) const { return (field(X, P) * rollingPhase + 1.0f) * 0.5f; }
    // 0x498d40 / 0x498db0: 10 px * field * sin((t + 1.2) * 0.5), added to x
    // and then (from the displaced x) to y.
    float wave(float X, float P) const { return 10.0f * field(X, P) * wavyPhase; }
};

}  // namespace tak::retailfog
