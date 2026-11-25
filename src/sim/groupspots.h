#pragma once
// Per-unit destinations for a group order (an engine feature kept in both
// pathfinding modes; docs/ta-port-plan.md "Known deviations").
//
// TotalA.exe's selection order (0x48cf30, sim/retailflyerspread.h) gives a
// unit within sqrt(3000 n) px of the group's centre the click plus its offset
// and every unit farther out the click itself, so several units of one order
// can share a point. The engine gives every unit it places its OWN spot
// instead (the "blue box" destination markers, GameView::drawUnitSpots):
//
//  1. Members within TA's reach keep TA's goal (the click plus their offset),
//     taken in member order, as long as their footprint there overlaps no
//     earlier spot and lies on the map -- so a group in formation keeps its
//     shape over the click exactly as in retail.
//  2. Every other placed member (out of reach, overlapping, or off the map)
//     takes the free footprint-sized spot nearest the click, on a 16 px grid
//     centred on it, nearest members first (squared distance, then member
//     order). A spot overlaps no other placed member's spot.
//
// Members with place == false count in the centre and get the click. Those with
// reserve set too (Legion's own planned movers: it slots them on the shared
// point itself, over the following ticks) hold a square around the click of
// three times their footprint area, which no placed spot overlaps, so a
// flyer's spot is not one of Legion's ground slots.
//
// Integer arithmetic on raw 16.16 positions only, so the sim (an explicit
// order's members, sim/matchsetup.cpp) and the client (from its frame, whose
// commands carry the goals) run the same rule; nothing here reads the world.
#include "sim/retailflyerspread.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <vector>

namespace ta::sim {

struct GroupSpotMember {
    int32_t x = 0, z = 0;    // raw 16.16 position
    int footX = 1, footZ = 1; // footprint, cells (16 px)
    bool place = true;       // false: counts in the centre, gets the click
    bool reserve = false;    // (place false) holds room around the click
};

// The goals (raw 16.16, member order) for a click at (clickX, clickZ). mapW /
// mapH: the map's extent in whole px (0: unbounded).
inline std::vector<std::array<int32_t, 2>> groupSpots(const std::vector<GroupSpotMember>& members,
                                                       int32_t clickX, int32_t clickZ,
                                                       int32_t mapW = 0, int32_t mapH = 0) {
    const size_t n = members.size();
    std::vector<std::array<int32_t, 2>> out(n, std::array<int32_t, 2>{clickX, clickZ});
    if (n == 0) return out;
    FlyerGroupCentre centre;
    for (const auto& m : members) centre.add(m.x, m.z);
    // A placed spot: its box in whole px [x0, x1) x [z0, z1).
    struct Box { int64_t x0, z0, x1, z1; };
    std::vector<Box> placed;
    placed.reserve(n);
    const auto half = [](int foot) { return int64_t(std::max(1, foot)) * 8; };
    const auto onMap = [&](const Box& b) {
        return (mapW <= 0 || (b.x0 >= 0 && b.x1 <= mapW)) && (mapH <= 0 || (b.z0 >= 0 && b.z1 <= mapH));
    };
    const auto overlaps = [&](const Box& b) {
        for (const auto& p : placed)
            if (b.x0 < p.x1 && p.x0 < b.x1 && b.z0 < p.z1 && p.z0 < b.z1) return true;
        return false;
    };
    // The room the reserving members hold, centred on the click.
    {
        int64_t held = 0;
        for (const auto& m : members)
            if (!m.place && m.reserve) held += 3 * int64_t(std::max(1, m.footX)) * std::max(1, m.footZ);
        if (held > 0) {
            int64_t side = 0;
            while (side * side < held) ++side;
            const int64_t cx = int64_t(clickX) >> 16, cz = int64_t(clickZ) >> 16;
            placed.push_back({cx - side * 8, cz - side * 8, cx + side * 8, cz + side * 8});
        }
    }
    // Pass 1: TA's goal for the members within its reach.
    std::vector<size_t> rest;
    for (size_t i = 0; i < n; ++i) {
        const auto& m = members[i];
        if (!m.place) continue;
        int64_t ox = 0, oz = 0;
        if (centre.offset(m.x, m.z, ox, oz)) {
            int32_t gx = 0, gz = 0;
            centre.goal(m.x, m.z, clickX, clickZ, gx, gz);
            // The box around the goal's whole-pixel centre.
            const int64_t cx = int64_t(gx) >> 16, cz = int64_t(gz) >> 16;
            const Box b{cx - half(m.footX), cz - half(m.footZ), cx + half(m.footX), cz + half(m.footZ)};
            if (onMap(b) && !overlaps(b)) {
                placed.push_back(b);
                out[i] = {gx, gz};
                continue;
            }
        }
        rest.push_back(i);
    }
    if (rest.empty()) return out;
    // Pass 2: the nearest free grid spot to the click, nearest members first.
    const int64_t px = int64_t(clickX) >> 16, pz = int64_t(clickZ) >> 16;
    const auto dist2 = [&](size_t i) {
        const int64_t dx = (int64_t(members[i].x) >> 16) - px, dz = (int64_t(members[i].z) >> 16) - pz;
        return dx * dx + dz * dz;
    };
    std::stable_sort(rest.begin(), rest.end(), [&](size_t a, size_t b) { return dist2(a) < dist2(b); });
    // The search reach in cells: room for every placed footprint four times
    // over, past the farthest TA offset, plus the biggest body.
    int64_t area = 0, foot = 1;
    for (const auto& m : members)
        if (m.place || m.reserve) {
            area += int64_t(std::max(1, m.footX)) * std::max(1, m.footZ) * (m.place ? 1 : 3);
            foot = std::max<int64_t>({foot, m.footX, m.footZ});
        }
    int64_t isq = 0;
    while ((isq + 1) * (isq + 1) <= 4 * area) ++isq;
    int64_t reach = 0;
    while ((reach + 1) * (reach + 1) <= 3000 * int64_t(n)) ++reach;
    const int64_t limit = std::min<int64_t>(isq + reach / 16 + 2 * foot + 4, 1024);
    // Occupied grid cells (16 px, the click's cell at the centre) in a window of
    // 2 * limit + 1 cells: every placed box marks the cells it touches, so a
    // grid-aligned box on free cells overlaps nothing placed.
    const int64_t side = 2 * limit + 1;
    const int64_t gx0 = px - limit * 16 - 8, gz0 = pz - limit * 16 - 8;   // the window's corner, px
    std::vector<uint8_t> used(size_t(side * side), 0);
    const auto mark = [&](const Box& b) {
        const int64_t i0 = std::max<int64_t>(0, (b.x0 - gx0) >= 0 ? (b.x0 - gx0) / 16 : -1);
        const int64_t j0 = std::max<int64_t>(0, (b.z0 - gz0) >= 0 ? (b.z0 - gz0) / 16 : -1);
        const int64_t i1 = std::min<int64_t>(side - 1, (b.x1 - 1 - gx0) >= 0 ? (b.x1 - 1 - gx0) / 16 : -1);
        const int64_t j1 = std::min<int64_t>(side - 1, (b.z1 - 1 - gz0) >= 0 ? (b.z1 - 1 - gz0) / 16 : -1);
        for (int64_t j = j0; j <= j1; ++j)
            for (int64_t i = i0; i <= i1; ++i) used[size_t(j * side + i)] = 1;
    };
    for (const auto& b : placed) mark(b);
    for (const size_t idx : rest) {
        const auto& m = members[idx];
        const int64_t fx = std::max(1, m.footX), fz = std::max(1, m.footZ);
        // A box whose first cell is (i, j): free and on the map?
        const auto fits = [&](int64_t i, int64_t j) {
            if (i < 0 || j < 0 || i + fx > side || j + fz > side) return false;
            const Box b{gx0 + i * 16, gz0 + j * 16, gx0 + (i + fx) * 16, gz0 + (j + fz) * 16};
            if (!onMap(b)) return false;
            for (int64_t y = j; y < j + fz; ++y)
                for (int64_t x = i; x < i + fx; ++x)
                    if (used[size_t(y * side + x)]) return false;
            return true;
        };
        // Rings of growing Chebyshev radius r around the box centred on the
        // click; the nearest free box (squared centre distance, then z, then x)
        // is final once no later ring can hold a nearer one (r^2 > best).
        const int64_t ci = limit - fx / 2, cj = limit - fz / 2;   // the box nearest centred on the click
        // Its centre's offset from the click in half px (even boxes sit half a cell off).
        const int64_t hx = (fx % 2) ? 0 : -8, hz = (fz % 2) ? 0 : -8;
        int64_t best = -1, bi = 0, bj = 0;
        const auto visit = [&](int64_t di, int64_t dj) {
            const int64_t ex = di * 16 + hx, ez = dj * 16 + hz, d = ex * ex + ez * ez;
            if (best >= 0 && (d > best || (d == best && (dj > bj - cj || (dj == bj - cj && di >= bi - ci)))))
                return;
            if (!fits(ci + di, cj + dj)) return;
            best = d; bi = ci + di; bj = cj + dj;
        };
        for (int64_t r = 0; r <= limit; ++r) {
            if (best >= 0 && (r - 1) * (r - 1) * 256 > best) break;
            if (r == 0) { visit(0, 0); continue; }
            for (int64_t di = -r; di <= r; ++di) { visit(di, -r); visit(di, r); }
            for (int64_t dj = -r + 1; dj <= r - 1; ++dj) { visit(-r, dj); visit(r, dj); }
        }
        if (best < 0) continue;   // no room in the window: it keeps the click
        const Box b{gx0 + bi * 16, gz0 + bj * 16, gx0 + (bi + fx) * 16, gz0 + (bj + fz) * 16};
        placed.push_back(b);
        mark(b);
        const int64_t cx = (b.x0 + b.x1) / 2, cz = (b.z0 + b.z1) / 2;
        out[idx] = {int32_t(std::clamp<int64_t>(cx * 65536, INT32_MIN, INT32_MAX)),
                    int32_t(std::clamp<int64_t>(cz * 65536, INT32_MIN, INT32_MAX))};
    }
    return out;
}

} // namespace ta::sim
