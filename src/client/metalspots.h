#pragma once

// Metal deposits as build spots for metal extractors, the way the TAK engine treats
// its lodestone deposits: an extractor snaps onto the nearest deposit, and a box
// dragged over the map queues one on every deposit inside it. Display / input only:
// it reads the sim's per-cell metal (World::cellMetal, docs/retail-economy.md section
// 5) and changes nothing -- the sim still accepts an extractor anywhere, as retail does.
//
// A TA map's metal is SurfaceMetal on every 16 px cell, overwritten under each
// indestructible metal feature (the metal patches, e.g. ArchMetal1 metal=187, 3x3).
// A deposit cell is one above SurfaceMetal; deposits are its 8-connected clusters.
// Each cluster gives the footprint position covering the most metal (an extractor's
// yield is ExtractsMetal x sum(cell metal + 1) over its footprint, 0x437840), and
// further non-overlapping positions only while they still earn at least kMoreShare
// of the best one's yield -- so a lone 3x3 patch is one 2x2 spot, not two half ones,
// and a field of patches side by side is one spot per patch.
//
// A metal planet -- SurfaceMetal at least kPlanetSurface (the Core-world campaign
// maps, Metal Gridlock and the lava maps: 20..255) or no deposit anywhere -- has no
// spots: extractors are placed freely there and Shift draws a line of them.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ta::client::metalspots {

inline constexpr int kPlanetSurface = 20;
inline constexpr float kMoreShare = 0.6f;
// How far from the pointer (world px) an extractor looks for a deposit to snap to.
inline constexpr float kSnapRadius = 128.0f;

struct Spot {
    int ox = 0, oz = 0;   // footprint origin, in 16 px cells
    int yield = 0;        // sum(cell metal + 1) over the footprint
    float x = 0, z = 0;   // the build point: footprintSnap's centre, origin * 16 + size * 8
};

struct Cells {
    const uint8_t* metal = nullptr;   // w x h, row-major
    int w = 0, h = 0;
    int surface = 0;
    int at(int x, int z) const { return x < 0 || z < 0 || x >= w || z >= h ? 0 : metal[size_t(z) * size_t(w) + size_t(x)]; }
    bool deposit(int x, int z) const { return x >= 0 && z >= 0 && x < w && z < h && at(x, z) > surface; }
};

inline bool anyDeposit(const Cells& c) {
    for (int z = 0; z < c.h; ++z)
        for (int x = 0; x < c.w; ++x)
            if (c.deposit(x, z)) return true;
    return false;
}

inline bool metalPlanet(const Cells& c) { return c.surface >= kPlanetSurface || !anyDeposit(c); }

// The spots for an extractor with an fx x fz footprint, in map order (top row first).
inline std::vector<Spot> find(const Cells& c, int fx, int fz) {
    std::vector<Spot> out;
    if (!c.metal || c.w <= 0 || c.h <= 0 || fx <= 0 || fz <= 0 || metalPlanet(c)) return out;
    std::vector<int> label(size_t(c.w) * size_t(c.h), -1);
    std::vector<int> stack;
    int clusters = 0;
    for (int z0 = 0; z0 < c.h; ++z0)
        for (int x0 = 0; x0 < c.w; ++x0) {
            if (!c.deposit(x0, z0) || label[size_t(z0) * size_t(c.w) + size_t(x0)] >= 0) continue;
            const int id = clusters++;
            int minX = x0, maxX = x0, minZ = z0, maxZ = z0;
            stack.assign(1, z0 * c.w + x0);
            label[size_t(z0) * size_t(c.w) + size_t(x0)] = id;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % c.w, z = i / c.w;
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, nz = z + dz;
                        if (!c.deposit(nx, nz)) continue;
                        int& l = label[size_t(nz) * size_t(c.w) + size_t(nx)];
                        if (l >= 0) continue;
                        l = id;
                        stack.push_back(nz * c.w + nx);
                    }
            }
            // Every origin whose footprint touches the cluster, scored by its yield;
            // the best first, then any that do not overlap one taken and still earn.
            struct Cand { int ox, oz, yield, cells; };
            std::vector<Cand> cands;
            for (int oz = minZ - fz + 1; oz <= maxZ; ++oz)
                for (int ox = minX - fx + 1; ox <= maxX; ++ox) {
                    if (ox < 0 || oz < 0 || ox + fx > c.w || oz + fz > c.h) continue;
                    int yield = 0, cells = 0;
                    for (int z = oz; z < oz + fz; ++z)
                        for (int x = ox; x < ox + fx; ++x) {
                            yield += c.at(x, z) + 1;
                            if (c.deposit(x, z) && label[size_t(z) * size_t(c.w) + size_t(x)] == id) ++cells;
                        }
                    if (cells > 0) cands.push_back({ox, oz, yield, cells});
                }
            // Best yield first; ties to the top-left, so the choice is the same everywhere.
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (a.yield != b.yield) return a.yield > b.yield;
                return a.oz != b.oz ? a.oz < b.oz : a.ox < b.ox;
            });
            std::vector<Cand> taken;
            for (const auto& k : cands) {
                if (!taken.empty() && float(k.yield) < kMoreShare * float(taken.front().yield)) break;
                const bool overlaps = std::any_of(taken.begin(), taken.end(), [&](const Cand& t) {
                    return k.ox < t.ox + fx && t.ox < k.ox + fx && k.oz < t.oz + fz && t.oz < k.oz + fz;
                });
                if (!overlaps) taken.push_back(k);
            }
            for (const auto& t : taken)
                out.push_back({t.ox, t.oz, t.yield, float(t.ox * 16 + fx * 8), float(t.oz * 16 + fz * 8)});
        }
    std::sort(out.begin(), out.end(), [](const Spot& a, const Spot& b) { return a.oz != b.oz ? a.oz < b.oz : a.ox < b.ox; });
    return out;
}

// The spot nearest (x, z) within kSnapRadius, or nullptr.
inline const Spot* nearest(const std::vector<Spot>& spots, float x, float z, float radius = kSnapRadius) {
    const Spot* best = nullptr;
    float bestD = radius * radius;
    for (const auto& s : spots) {
        const float d = (s.x - x) * (s.x - x) + (s.z - z) * (s.z - z);
        if (d <= bestD) { bestD = d; best = &s; }
    }
    return best;
}

// The spots whose build point lies in the box (x0, z0)-(x1, z1), any corner order,
// ordered as one builder walks them: from (fromX, fromZ) always to the nearest left.
inline std::vector<Spot> inBox(const std::vector<Spot>& spots, float x0, float z0, float x1, float z1,
                               float fromX, float fromZ) {
    const float lx = std::min(x0, x1), hx = std::max(x0, x1), lz = std::min(z0, z1), hz = std::max(z0, z1);
    std::vector<Spot> left;
    for (const auto& s : spots)
        if (s.x >= lx && s.x <= hx && s.z >= lz && s.z <= hz) left.push_back(s);
    std::vector<Spot> out;
    float px = fromX, pz = fromZ;
    while (!left.empty()) {
        size_t bi = 0;
        float bd = 1e30f;
        for (size_t i = 0; i < left.size(); ++i) {
            const float d = (left[i].x - px) * (left[i].x - px) + (left[i].z - pz) * (left[i].z - pz);
            if (d < bd) { bd = d; bi = i; }
        }
        out.push_back(left[bi]);
        px = left[bi].x; pz = left[bi].z;
        left.erase(left.begin() + std::ptrdiff_t(bi));
    }
    return out;
}

}  // namespace ta::client::metalspots
