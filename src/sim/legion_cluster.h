#pragma once
// legion_cluster.h -- the clusters of a static sync's dirty rectangles (Legion's refreshPlane, src/sim/legion.cpp).
//
// refreshPlane refreshes changes far apart one cluster at a time. Two rectangles {x,z,w,h} are NEIGHBOURS when their
// boxes grown by `grow` on every side overlap:
//     a.x-grow < b.x+b.w+grow && b.x-grow < a.x+a.w+grow && a.z-grow < b.z+b.h+grow && b.z-grow < a.z+a.h+grow
// and a cluster is a connected set of the neighbour relation (transitive). Clusters are listed in the order of their
// first (lowest-index) rectangle, and a cluster's rectangles in index order. This is the pure function refreshPlane
// computed with a test of every pair and a union-find whose root is always the component's lowest index; here the
// same partition comes from a test of every pair (few rectangles) or from bins (many), so the result is identical
// whichever path runs (legion_cluster_test checks both against a brute-force reference).
//
// The binned path. A grown box with positive extent on both axes is a set of integer cells [x-grow, x+w+grow) x
// [z-grow, z+h+grow), and two such boxes are neighbours exactly when they share a cell (the test above is the
// half-open interval overlap, whose smallest common point is a cell of both). The cells are cut into 8x8 bins; in
// each bin a box covers a 64-bit mask. A bin keeps a list of entries {mask, rectangle}, the masks pairwise disjoint:
// a new box joins (unions with) every entry its mask meets, and those entries and its mask become one entry. Every
// pair that shares a cell shares it in some bin, where the later one meets the entry holding the earlier one's
// cells (entries only merge, so that entry's rectangle is already joined to the earlier one); a join is made only
// on a shared cell, so only neighbours are joined. A bin holds at most 64 entries, so a box costs its bins times at
// most 64 mask tests, however many rectangles pile up in one place (the pairwise test costs n*n/2 for n of them).
// A box with no positive extent on some axis (never produced by the engine: grow >= 1 and footprints are >= 0),
// or one covering more bins than kMaxBinsPerRect, is joined by the pairwise test against every rectangle instead,
// which is exact for any integers.
//
// Derived from the rectangle list alone; no state, never hashed.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::sim::cluster {

using Rect = std::array<int, 4>;   // {x, z, w, h}

// The neighbour test, verbatim from refreshPlane (int arithmetic as there).
inline bool neighbours(const Rect& a, const Rect& b, int grow) {
    return a[0] - grow < b[0] + b[2] + grow && b[0] - grow < a[0] + a[2] + grow &&
           a[1] - grow < b[1] + b[3] + grow && b[1] - grow < a[1] + a[3] + grow;
}

struct Clusters {
    // Rectangle indices grouped by cluster: cluster k is order[start[k] .. start[k+1]), start.back() == n.
    std::vector<int> order, start;
    // Scratch, kept to reuse its storage.
    std::vector<int> parent, root, count, slot, irregular, head;
    struct Entry {uint64_t mask; int rect, next;};
    std::vector<Entry> entries;
    std::size_t clusters() const {return start.empty() ? 0 : start.size() - 1;}
};

// Up to this many rectangles the pairwise test is used: a box costs about 16 bin visits (grow 10-12), and on lists
// whose pair tests predict well (blobs listed together, clusterchurn-mid) the pairwise test stays cheaper up to
// about this size (legion_cluster_test --bench, --bench-scn).
inline constexpr std::size_t kPairwiseMax = 64;
inline constexpr int kBinShift = 3;                // 8 x 8 cells per bin: a 64-bit mask
inline constexpr long long kMaxBinsPerRect = 256;  // a larger box joins pairwise (exact, O(n) for that box)
inline constexpr long long kMaxBins = 1ll << 22;   // a bin grid larger than this: pairwise for every rectangle

namespace detail {
inline int find(std::vector<int>& p, int i) {
    while (p[std::size_t(i)] != i) i = p[std::size_t(i)] = p[std::size_t(p[std::size_t(i)])];
    return i;
}
// The lower root stays the root: a component's root is its lowest index whatever the order of the joins.
inline void join(std::vector<int>& p, int a, int b) {
    const int ra = find(p, a), rb = find(p, b);
    if (ra != rb) p[std::size_t(std::max(ra, rb))] = std::min(ra, rb);
}
inline void pairwise(const std::vector<Rect>& rects, int grow, std::vector<int>& p) {
    for (std::size_t i = 0; i < rects.size(); ++i)
        for (std::size_t j = i + 1; j < rects.size(); ++j)
            if (neighbours(rects[i], rects[j], grow)) join(p, int(i), int(j));
}
}  // namespace detail

// Fills `out` (see Clusters). `forceBins` (tests only) takes the binned path for any count.
inline void clusterRects(const std::vector<Rect>& rects, int grow, Clusters& out, bool forceBins = false) {
    const std::size_t n = rects.size();
    auto& p = out.parent;
    p.resize(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = int(i);
    bool binned = false;
    if (n > kPairwiseMax || (forceBins && n > 0)) {
        // Grown boxes as closed cell ranges [x0, x1] x [z0, z1] (64-bit: no overflow for any int input).
        auto& irr = out.irregular;
        irr.clear();
        long long gx0 = 0, gz0 = 0, gx1 = -1, gz1 = -1;
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            const Rect& r = rects[i];
            const long long x0 = (long long)r[0] - grow, x1 = (long long)r[0] + r[2] + grow - 1;
            const long long z0 = (long long)r[1] - grow, z1 = (long long)r[1] + r[3] + grow - 1;
            if (x1 < x0 || z1 < z0) continue;
            if (!any) {gx0 = x0; gz0 = z0; gx1 = x1; gz1 = z1; any = true; continue;}
            gx0 = std::min(gx0, x0); gz0 = std::min(gz0, z0); gx1 = std::max(gx1, x1); gz1 = std::max(gz1, z1);
        }
        const long long bw = any ? ((gx1 - gx0) >> kBinShift) + 1 : 0, bh = any ? ((gz1 - gz0) >> kBinShift) + 1 : 0;
        if (bw * bh <= kMaxBins) {
            binned = true;
            auto& head = out.head;
            head.assign(std::size_t(bw * bh), -1);
            auto& entries = out.entries;
            entries.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const Rect& r = rects[i];
                const long long x0 = (long long)r[0] - grow - gx0, x1 = (long long)r[0] + r[2] + grow - 1 - gx0;
                const long long z0 = (long long)r[1] - grow - gz0, z1 = (long long)r[1] + r[3] + grow - 1 - gz0;
                if (x1 < x0 || z1 < z0) {irr.push_back(int(i)); continue;}
                const long long bx0 = x0 >> kBinShift, bx1 = x1 >> kBinShift, bz0 = z0 >> kBinShift, bz1 = z1 >> kBinShift;
                if ((bx1 - bx0 + 1) * (bz1 - bz0 + 1) > kMaxBinsPerRect) {irr.push_back(int(i)); continue;}
                for (long long bz = bz0; bz <= bz1; ++bz) {
                    const int lz0 = int(std::max(z0, bz << kBinShift) - (bz << kBinShift));
                    const int lz1 = int(std::min(z1, (bz << kBinShift) + 7) - (bz << kBinShift));
                    const uint64_t rows = (0x0101010101010101ull >> (8 * (7 - (lz1 - lz0)))) << (8 * lz0);   // bit 0 of rows lz0..lz1
                    for (long long bx = bx0; bx <= bx1; ++bx) {
                        const int lx0 = int(std::max(x0, bx << kBinShift) - (bx << kBinShift));
                        const int lx1 = int(std::min(x1, (bx << kBinShift) + 7) - (bx << kBinShift));
                        const uint64_t row = ((uint64_t(2) << (lx1 - lx0)) - 1) << lx0;
                        uint64_t mask = row * rows;   // row < 256: no carries
                        int& h = head[std::size_t(bz * bw + bx)];
                        // Join every entry the box meets and fold it into the box's entry (masks stay disjoint).
                        int* link = &h;
                        while (*link >= 0) {
                            auto& e = entries[std::size_t(*link)];
                            if (e.mask & mask) {
                                detail::join(p, int(i), e.rect);
                                mask |= e.mask;
                                *link = e.next;
                            } else {
                                link = &e.next;
                            }
                        }
                        entries.push_back({mask, int(i), h});
                        h = int(entries.size() - 1);
                    }
                }
            }
            // Boxes the bins do not hold: the pairwise test against every other rectangle.
            for (const int i : irr)
                for (std::size_t j = 0; j < n; ++j)
                    if (int(j) != i && neighbours(rects[std::size_t(i)], rects[j], grow)) detail::join(p, i, int(j));
        }
    }
    if (!binned) detail::pairwise(rects, grow, p);
    // Group by root (= the cluster's first index), roots ascending, indices ascending within: a counting sort.
    auto& root = out.root;
    auto& count = out.count;
    root.resize(n);
    count.assign(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) ++count[std::size_t(root[i] = detail::find(p, int(i)))];
    out.start.clear();
    auto& slot = out.slot;
    slot.resize(n);
    int at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (root[i] != int(i)) continue;
        out.start.push_back(at);
        slot[i] = at;
        at += count[i];
    }
    out.start.push_back(at);
    out.order.resize(n);
    for (std::size_t i = 0; i < n; ++i) out.order[std::size_t(slot[std::size_t(root[i])]++)] = int(i);
}

}  // namespace tak::sim::cluster
