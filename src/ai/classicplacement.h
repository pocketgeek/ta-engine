#pragma once

// The Classic level's building placement (docs/classic-ai-plan.md §1.10): where
// retail's constructor planner puts what it picked. 0x412f80 sends a type in the
// lodestone draw class (yardmap[0] & 0x80) to the nearest free deposit 0x411510
// and everything else to the block-grid scatter 0x411200, then turns the chosen
// top-left cell into the footprint's centre.
//
// The scatter runs on a per-player pair of block grids that the build manager's
// constructor draws once (0x4110f0): one for land types, a coarser one for water
// types. A site is a random distance and angle from the base centre, snapped into
// a random spot of the grid block it falls in, which leaves free "streets" between
// buildings in neighbouring blocks; it must pass the placement test and must not
// cover a sacred deposit. Towers are drawn further out, so they ring the base.
//
// Pure templates over the host's random stream, placement test and feature plane,
// so the same code is what ClassicBrain runs on the World and what
// tools/re/check_ai_placement.py checks against retail's 0x4110f0, 0x412f80,
// 0x411200 and 0x411510 in emulation (with retail's own 0x5360bf/0x5360f3 trig
// and its 0x50e600 cell lookup underneath).
//
// Server-side only, like the rest of the brain.

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "sim/retailmotion.h"

namespace tak::ai {

// One block grid (build manager +0x105 land, +0x111 water): block steps in cells,
// the grid's offset, and the margin a building keeps from its block's far edge.
struct ClassicBlockGrid {
    int16_t stepX = 0, stepZ = 0, offX = 0, offZ = 0;
    int32_t margin = 0;
};
struct ClassicBlockGrids { ClassicBlockGrid land, water; };

// 0x4110f0: the land grid (margin 6, steps 14..23 by 14..16 cells) then the water
// grid (margin 9, steps 17..36 by 17..19), each with an offset of
// rand(step) - step/2 per axis: eight draws in that order.
template<class Random>
ClassicBlockGrids classicDrawBlockGrids(Random random) {
    auto grid = [&](int32_t margin, int x, int z) {
        ClassicBlockGrid g;
        g.margin = margin;
        g.stepX = int16_t(int32_t(random(x)) + int32_t(int16_t(margin)) + 8);
        g.stepZ = int16_t(int32_t(random(z)) + int32_t(int16_t(margin)) + 8);
        g.offX = int16_t(int32_t(random(g.stepX)) - g.stepX / 2);
        g.offZ = int16_t(int32_t(random(g.stepZ)) - g.stepZ / 2);
        return g;
    };
    ClassicBlockGrids grids;
    grids.land = grid(6, 10, 3);
    grids.water = grid(9, 20, 3);
    return grids;
}

// What the placer reads of the type it places.
struct ClassicSiteType {
    int16_t footX = 1, footZ = 1;   // type +0x126 / +0x128
    int16_t minWaterDepth = -10000; // type +0x194: >= 0 places on the water grid
    // 0x411200's "tower": bmcode 0, armed, first weapon's range under 1000 and no
    // build menu. Its distance from the centre is drawn so it rings the base.
    bool tower = false;
    // The lodestone draw class (yardmap[0] & 0x80): placed on a deposit instead.
    bool lodestone = false;
};

// 0x411200's tower test on a type: bmcode (type +0x24a) 0, armed (type +0x260
// 0x10000), its first weapon's range (type +0x1aa -> +0x90) under 1000, and no
// build menu (type +0x12e == 0).
inline bool classicSiteTower(uint8_t bmcode, bool armed, int32_t firstWeaponRange, int32_t menu) {
    return bmcode == 0 && armed && firstWeaponRange < 1000 && menu == 0;
}

// A feature type's sacredsite (feature type +0x134, a float) as the placers test
// it: fcomp against 0.0 with the "equal" flag, so 0 and NaN both mean "not sacred".
inline bool classicSacredValue(float sacredsite) { return sacredsite < 0.0f || sacredsite > 0.0f; }

// A top-left cell's footprint centre in 16.16 pixels, as 0x412f80 returns it:
// ((2 * cell + foot) * 8) pixels per axis.
inline std::pair<int32_t, int32_t> classicSiteCentre(std::pair<int16_t, int16_t> cell, int16_t footX, int16_t footZ) {
    return {int32_t(uint32_t(int32_t(footX) + 2 * int32_t(cell.first)) << 19),
            int32_t(uint32_t(int32_t(footZ) + 2 * int32_t(cell.second)) << 19)};
}

// The top-left cell of a footprint centred on a 16.16 point (the placers' own
// rounding: (p - foot * 8 px + 8 px) >> 4 px, arithmetic).
inline int16_t classicFootprintCell(int32_t p, int16_t foot) {
    return int16_t(int32_t(uint32_t(p) - (uint32_t(int32_t(foot)) << 19) + 0x80000u) >> 20);
}

// One map cell's feature (cell +8) as 0x411200 / 0x411510 resolve it: an index
// below 0xfffa names a feature type, if it is below the type count; 0xfffe is a
// footprint tail that points back (cell +0xb columns, +0xa rows) at its anchor,
// whose index is used if it is below 0xfffa (no count check on that path); every
// other marker is no feature. `cellAt(x, z)` gives {feature, backX, backZ} of a
// cell, `sacred(index)` whether that feature type's sacredsite is non-zero.
struct ClassicFeatureCell { uint16_t feature = 0xffff; uint8_t backX = 0, backZ = 0; };
template<class CellAt, class Sacred>
bool classicSacredCell(int x, int z, uint32_t featureTypes, CellAt cellAt, Sacred sacred) {
    const ClassicFeatureCell c = cellAt(x, z);
    if (c.feature < 0xfffa) return c.feature < featureTypes && sacred(c.feature);
    if (c.feature != 0xfffe) return false;
    const ClassicFeatureCell anchor = cellAt(x - c.backX, z - c.backZ);
    return anchor.feature < 0xfffa && sacred(anchor.feature);
}

// 0x411200, the ordinary placer: up to 30 attempts around `centre` (16.16) within
// `radius` pixels. Each attempt draws a distance -- rand(R), or for a tower
// rand(R/2)+rand(R/2)+rand(R/2) drawn again while it is over R -- and an angle
// rand(65536), takes the point centre + (-d sin a, -d cos a) through retail's
// scaled sine and cosine, finds the top-left cell of the footprint centred
// there, and moves it into its grid block: trunc(cell / step) * step + offset +
// rand(step - foot - margin) per axis. `valid(x, z)` is the placement test
// 0x507d10(type, 0, cell, 1, 0); a site that passes is then refused if any
// footprint cell holds a sacred feature (`sacred(x, z)`, classicSacredCell).
// The first site that passes both is the result.
template<class Random, class Valid, class Sacred>
std::optional<std::pair<int16_t, int16_t>> classicScatterSite(const ClassicBlockGrids& grids,
        std::pair<int32_t, int32_t> centre, int32_t radius, const ClassicSiteType& type,
        Random random, Valid valid, Sacred sacred) {
    const ClassicBlockGrid& g = type.minWaterDepth < 0 ? grids.land : grids.water;
    for (int attempt = 0; attempt < 30; ++attempt) {
        int32_t d;
        if (type.tower) {
            const int32_t half = radius / 2;
            do d = int32_t(random(half)) + int32_t(random(half)) + int32_t(random(half));
            while (d > radius);
        } else {
            d = int32_t(random(radius));
        }
        const int32_t magnitude = int32_t(uint32_t(d) << 16);
        const uint16_t angle = uint16_t(random(0x10000));
        const int32_t x = int32_t(uint32_t(centre.first) - uint32_t(sim::retailScaledSine(angle, magnitude)));
        const int32_t z = int32_t(uint32_t(centre.second) - uint32_t(sim::retailScaledCosine(angle, magnitude)));
        const int16_t cx = classicFootprintCell(x, type.footX), cz = classicFootprintCell(z, type.footZ);
        const int32_t jitterX = int32_t(random(int32_t(g.stepX) - type.footX - g.margin));
        const int16_t sx = int16_t(uint16_t(uint32_t((int32_t(cx) / g.stepX) * g.stepX) + uint32_t(g.offX) +
                                            uint32_t(jitterX)));
        const int32_t jitterZ = int32_t(random(int32_t(g.stepZ) - type.footZ - g.margin));
        const int16_t sz = int16_t(uint16_t(uint32_t((int32_t(cz) / g.stepZ) * g.stepZ) + uint32_t(g.offZ) +
                                            uint32_t(jitterZ)));
        if (!valid(sx, sz)) continue;
        bool covered = false;
        for (int dz = 0; dz < type.footZ; ++dz)
            for (int dx = 0; dx < type.footX; ++dx)
                if (sacred(int(sx) + dx, int(sz) + dz)) covered = true;
        if (!covered) return std::pair{sx, sz};
    }
    return std::nullopt;
}

// 0x411510, the lodestone placer: of the sacred-site records (0x62dbcc, each its
// anchor cell, or none for an empty record), the nearest one to the top-left cell
// of a footprint centred on `centre` -- sqrt(dx^2 + dz^2) in 16-pixel cells x 16,
// truncated -- that is within `radius`, still holds a sacred feature
// (`holds(x, z)`: classicSacredCell, false off the map) and passes the placement
// test `valid(x, z)` (asked only for a site nearer than the best so far). Ties keep
// the first record. The lodestone's top-left cell goes on the site's cell.
template<class Holds, class Valid>
std::optional<std::pair<int16_t, int16_t>> classicDepositSite(
        const std::vector<std::optional<std::pair<int16_t, int16_t>>>& sites, std::pair<int32_t, int32_t> centre,
        int32_t radius, int16_t footX, int16_t footZ, Holds holds, Valid valid) {
    const int16_t ox = classicFootprintCell(centre.first, footX), oz = classicFootprintCell(centre.second, footZ);
    int32_t best = int32_t(uint32_t(radius) * 2u);
    std::optional<std::pair<int16_t, int16_t>> found;
    for (const auto& site : sites) {
        if (!site) continue;
        const double dz = double(int32_t(uint32_t(int32_t(oz) - int32_t(site->second)) << 4));
        const double dx = double(int32_t(uint32_t(int32_t(ox) - int32_t(site->first)) << 4));
        const int32_t d = int32_t(uint32_t(uint64_t(int64_t(std::sqrt(dx * dx + dz * dz)))));
        if (d > radius) continue;
        if (!holds(site->first, site->second)) continue;
        if (d >= best || !valid(site->first, site->second)) continue;
        best = d;
        found = *site;
    }
    return found;
}

// 0x412f80: the site of `type` for a base centred on `centre` within `radius`, as
// a footprint centre in 16.16 pixels. `deposit()` runs classicDepositSite and
// `scatter()` classicScatterSite, each returning the chosen top-left cell.
template<class Deposit, class Scatter>
std::optional<std::pair<int32_t, int32_t>> classicPlaceSite(const ClassicSiteType& type, Deposit deposit,
                                                           Scatter scatter) {
    const std::optional<std::pair<int16_t, int16_t>> cell = type.lodestone ? deposit() : scatter();
    if (!cell) return std::nullopt;
    return classicSiteCentre(*cell, type.footX, type.footZ);
}

}  // namespace tak::ai
