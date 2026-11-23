#pragma once
// legion_routebins.h -- the route invalidation index of Legion's navigator (perf follow-up I, 2026-10-30).
//
// A static change (legion.cpp syncStatic -> staticChanged) arrives as one box per changed cluster and movement plane.
// It reaches a route (a group's field or its pending refresh, `next`) when touches() says so: the box comes within one
// cell of the route's window and, for a corridor or a tile chain, of one of its listed tiles. staticChanged used to ask
// that of every group for every change. This index lists, per movement plane, a grid of square bins (kShift: 32 x 32
// cells) and in each bin the groups one of whose routes covers some cell the bin holds, the cells grown by one first
// (the reach of touches()). A change box then names the bins it overlaps, and the groups listed there are every group
// it can reach, and usually few more: touches() still decides for each of them, in ascending group id order, so
// staticChanged does exactly what the walk of every group did.
//
// Why the listing is enough (route boxes are inclusive cell boxes; "grown" = one cell more on every side):
//  * a window route (no tile list) is reached exactly when the change box overlaps its window grown by one; the
//    grown window is what it lists (a window of no cells still lists its one-cell ring, as touches() reads it);
//  * a corridor or chain is reached only when the change box grown by one overlaps an allowed tile inside the window
//    (touches() scans exactly those tiles); so the box overlaps that tile's part of the window, grown by one, and
//    that is what is listed, tile by tile.
// A bin index is clamped onto the grid (monotone), so a box that overlaps a listed rectangle always shares a bin with
// it, at the map's edges too.
//
// Registration replaces a group's previous one (list), and a group without routes is unlisted (unlist). Removal is
// lazy: an entry carries its group's registration generation, which an unlist moves on, so a stale entry costs
// nothing until a query skips it (and drops it from that bin) or a compaction (when stale entries outnumber live ones
// by kCompactSlack) sweeps every bin. The index is derived state: it is never hashed and decides nothing (it only
// proposes candidates); its memory is reported by bytes() and is not part of NavBytes.
#include "sim/legion_route.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::sim::routebins {

// Does a static change box (inclusive cells x0,z0,x1,z1; x1 < 0: none) reach the route? A route covers its window
// (its seeds' component box, or a margin round its bodies); a change farther than one cell from every cell of that box
// cannot alter any step inside it (a step reads its target and the two orthogonal cells), nor merge another region
// into it. A corridor or chain (stage 6) holds only its listed tiles, so a change one cell or more from all of them
// reaches none of its steps (the same argument as the box): only those tiles are tested.
inline bool touches(const RouteGrid& f, const std::array<int, 4>& b) {
    if (b[2] < 0) return false;
    if (!(b[0] <= f.x0 + f.fw && b[2] >= f.x0 - 1 && b[1] <= f.z0 + f.fh && b[3] >= f.z0 - 1)) return false;
    if (f.allow.empty()) return true;
    const int x0 = std::max(b[0] - 1, f.x0), z0 = std::max(b[1] - 1, f.z0), x1 = std::min(b[2] + 1, f.x0 + f.fw - 1),
              z1 = std::min(b[3] + 1, f.z0 + f.fh - 1);
    if (x0 > x1 || z0 > z1) return false;
    for (int sz = z0 >> f.shift; sz <= z1 >> f.shift; ++sz)
        for (int sx = x0 >> f.shift; sx <= x1 >> f.shift; ++sx)
            if (f.allow[size_t(sz - f.sz0) * size_t(f.sw) + size_t(sx - f.sx0)]) return true;
    return false;
}

// The cell boxes (inclusive x0,z0,x1,z1) a change box must overlap to reach the route: see the header comment.
template <class F> void forEachRect(const RouteGrid& f, F&& fn) {
    if (f.allow.empty()) {
        fn(f.x0 - 1, f.z0 - 1, f.x0 + f.fw, f.z0 + f.fh);
        return;
    }
    const int k = 1 << f.shift;
    for (int sz = 0; sz < f.sh; ++sz)
        for (int sx = 0; sx < f.sw; ++sx) {
            if (!f.allow[size_t(sz) * size_t(f.sw) + size_t(sx)]) continue;
            const int tx = (f.sx0 + sx) * k, tz = (f.sz0 + sz) * k;
            const int x0 = std::max(tx, f.x0), z0 = std::max(tz, f.z0);
            const int x1 = std::min(tx + k - 1, f.x0 + f.fw - 1), z1 = std::min(tz + k - 1, f.z0 + f.fh - 1);
            if (x0 > x1 || z0 > z1) continue;
            fn(x0 - 1, z0 - 1, x1 + 1, z1 + 1);
        }
}

class Index {
public:
    static constexpr int kShift = 5;                      // bins of 32 x 32 cells (the default sector size)
    static constexpr uint64_t kCompactSlack = 1u << 16;  // stale entries tolerated beyond the live ones

    // Observation only (never hashed, read by no decision).
    struct Stats {
        uint64_t lists = 0, unlists = 0, entriesAdded = 0;       // registrations, removals, bin entries written
        uint64_t queries = 0, binsScanned = 0, entriesScanned = 0, staleDropped = 0;   // gather()
        uint64_t compactions = 0, compacted = 0;                   // whole-index sweeps and the entries they dropped
        uint64_t entriesPeak = 0;                                  // the most entries (live + stale) at once
    };

    // Size the grid for a map of w x h cells. A new size drops every registration and returns true (the caller lists
    // its groups again); false when nothing changed. A map of no cells leaves the index unusable (usable()).
    bool size(int w, int h) {
        if (w == w_ && h == h_) return false;
        w_ = w; h_ = h;
        bw_ = w > 0 ? ((w - 1) >> kShift) + 1 : 0;
        bh_ = h > 0 ? ((h - 1) >> kShift) + 1 : 0;
        planes_.clear();
        mark_.assign(size_t(bw_) * size_t(bh_), 0);
        markGen_ = 0;
        for (size_t id = 0; id < gen_.size(); ++id)
            if (gen_[id] & 1) ++gen_[id];
        count_.assign(count_.size(), 0);
        live_ = stale_ = 0;
        return true;
    }
    bool usable() const { return bw_ > 0 && bh_ > 0; }
    int binsAcross() const { return bw_; }
    int binsDown() const { return bh_; }

    // List group `id` on movement plane `plane` for the routes a and b (either may be null), replacing its previous
    // listing. A group with no route, or no plane, is only unlisted.
    void list(int id, int plane, const RouteGrid* a, const RouteGrid* b) {
        unlist(id);
        if (id < 0 || plane < 0 || (!a && !b) || !usable()) return;
        if (size_t(plane) >= planes_.size()) planes_.resize(size_t(plane) + 1);
        auto& bins = planes_[size_t(plane)];
        if (bins.empty()) bins.resize(size_t(bw_) * size_t(bh_));
        grow(id);
        const uint32_t g = ++gen_[size_t(id)];   // odd: listed (unlist left it even)
        if (++markGen_ == 0) {
            std::fill(mark_.begin(), mark_.end(), 0);
            markGen_ = 1;
        }
        uint32_t n = 0;
        auto put = [&](int x0, int z0, int x1, int z1) {
            const int bx0 = binX(x0), bx1 = binX(x1), bz0 = binZ(z0), bz1 = binZ(z1);
            for (int bz = bz0; bz <= bz1; ++bz)
                for (int bx = bx0; bx <= bx1; ++bx) {
                    const size_t k = size_t(bz) * size_t(bw_) + size_t(bx);
                    if (mark_[k] == markGen_) continue;
                    mark_[k] = markGen_;
                    bins[k].push_back({id, g});
                    ++n;
                }
        };
        for (const RouteGrid* f : {a, b})
            if (f) forEachRect(*f, put);
        count_[size_t(id)] = n;
        live_ += n;
        ++stats_.lists;
        stats_.entriesAdded += n;
        stats_.entriesPeak = std::max(stats_.entriesPeak, live_ + stale_);
    }
    void unlist(int id) {
        if (id < 0 || size_t(id) >= gen_.size() || !(gen_[size_t(id)] & 1)) return;
        ++gen_[size_t(id)];   // even: every entry of the old listing is stale
        live_ -= count_[size_t(id)];
        stale_ += count_[size_t(id)];
        count_[size_t(id)] = 0;
        ++stats_.unlists;
        if (stale_ > live_ + kCompactSlack) compact();
    }
    bool listed(int id) const { return id >= 0 && size_t(id) < gen_.size() && (gen_[size_t(id)] & 1); }

    // A query: begin(), then gather() once per change box; `out` receives each listed group once per query, in no
    // particular order (the caller sorts). estimate() is the number of entries gather() would read for the box.
    void begin() {
        if (++seenGen_ == 0) {
            std::fill(seen_.begin(), seen_.end(), 0);
            seenGen_ = 1;
        }
    }
    size_t estimate(int plane, const std::array<int, 4>& b) const {
        if (!usable() || plane < 0 || size_t(plane) >= planes_.size() || planes_[size_t(plane)].empty() || b[2] < b[0] ||
            b[3] < b[1])
            return 0;
        const auto& bins = planes_[size_t(plane)];
        size_t n = 0;
        for (int bz = binZ(b[1]); bz <= binZ(b[3]); ++bz)
            for (int bx = binX(b[0]); bx <= binX(b[2]); ++bx) n += bins[size_t(bz) * size_t(bw_) + size_t(bx)].size();
        return n;
    }
    void gather(int plane, const std::array<int, 4>& b, std::vector<int>& out) {
        if (!usable() || plane < 0 || size_t(plane) >= planes_.size() || planes_[size_t(plane)].empty() || b[2] < b[0] ||
            b[3] < b[1])
            return;
        ++stats_.queries;
        auto& bins = planes_[size_t(plane)];
        for (int bz = binZ(b[1]); bz <= binZ(b[3]); ++bz)
            for (int bx = binX(b[0]); bx <= binX(b[2]); ++bx) {
                auto& v = bins[size_t(bz) * size_t(bw_) + size_t(bx)];
                ++stats_.binsScanned;
                stats_.entriesScanned += v.size();
                size_t keep = 0;
                for (size_t i = 0; i < v.size(); ++i) {
                    const Entry e = v[i];
                    if (gen_[size_t(e.id)] != e.gen) continue;   // a stale entry: dropped here
                    v[keep++] = e;
                    if (seen_[size_t(e.id)] != seenGen_) {
                        seen_[size_t(e.id)] = seenGen_;
                        out.push_back(e.id);
                    }
                }
                stale_ -= v.size() - keep;
                stats_.staleDropped += v.size() - keep;
                v.resize(keep);
            }
    }

    // Verification (TAK_LEGION_VERIFY, tests): gather()'s candidates for a box, appended in no particular order and
    // possibly repeated, without changing the index (no stale entry dropped, no stats): a check must not move what
    // the index does next (staticChanged's choice between the index and the walk reads the bins' sizes).
    void collect(int plane, const std::array<int, 4>& b, std::vector<int>& out) const {
        if (!usable() || plane < 0 || size_t(plane) >= planes_.size() || planes_[size_t(plane)].empty() || b[2] < b[0] ||
            b[3] < b[1])
            return;
        const auto& bins = planes_[size_t(plane)];
        for (int bz = binZ(b[1]); bz <= binZ(b[3]); ++bz)
            for (int bx = binX(b[0]); bx <= binX(b[2]); ++bx)
                for (const Entry& e : bins[size_t(bz) * size_t(bw_) + size_t(bx)])
                    if (gen_[size_t(e.id)] == e.gen) out.push_back(e.id);
    }
    // The groups listed live in one bin, ascending; and whether the live and stale counts agree with the entries held.
    std::vector<int> liveIds(int plane, int bin) const {
        std::vector<int> ids;
        if (plane < 0 || size_t(plane) >= planes_.size() || planes_[size_t(plane)].empty()) return ids;
        for (const Entry& e : planes_[size_t(plane)][size_t(bin)])
            if (gen_[size_t(e.id)] == e.gen) ids.push_back(e.id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    bool countsAgree() const {
        uint64_t live = 0, all = 0;
        for (const auto& bins : planes_)
            for (const auto& v : bins) {
                all += v.size();
                for (const Entry& e : v) live += gen_[size_t(e.id)] == e.gen;
            }
        return live == live_ && all - live == stale_;
    }
    // The bins a group listed with these routes would occupy (ascending bin indices): what list() writes.
    std::vector<int> binsOf(const RouteGrid* a, const RouteGrid* b) const {
        std::vector<int> out;
        if (!usable()) return out;
        auto put = [&](int x0, int z0, int x1, int z1) {
            for (int bz = binZ(z0); bz <= binZ(z1); ++bz)
                for (int bx = binX(x0); bx <= binX(x1); ++bx) out.push_back(bz * bw_ + bx);
        };
        for (const RouteGrid* f : {a, b})
            if (f) forEachRect(*f, put);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    const Stats& stats() const { return stats_; }
    uint64_t liveEntries() const { return live_; }
    uint64_t staleEntries() const { return stale_; }
    // Bytes held (the bins' storage and the per-group tables), for the memory report.
    uint64_t bytes() const {
        uint64_t n = (gen_.capacity() + count_.capacity() + seen_.capacity() + mark_.capacity()) * sizeof(uint32_t) +
                     planes_.capacity() * sizeof(std::vector<std::vector<Entry>>);
        for (const auto& bins : planes_) {
            n += bins.capacity() * sizeof(std::vector<Entry>);
            for (const auto& v : bins) n += v.capacity() * sizeof(Entry);
        }
        return n;
    }

private:
    struct Entry {
        int id;
        uint32_t gen;
    };
    int binX(int x) const { return x < 0 ? 0 : std::min(x >> kShift, bw_ - 1); }
    int binZ(int z) const { return z < 0 ? 0 : std::min(z >> kShift, bh_ - 1); }
    void grow(int id) {
        if (size_t(id) < gen_.size()) return;
        const size_t n = std::max(size_t(id) + 1, gen_.size() * 2);
        gen_.resize(n, 0);
        count_.resize(n, 0);
        seen_.resize(n, 0);
    }
    void compact() {
        ++stats_.compactions;
        stats_.compacted += stale_;
        for (auto& bins : planes_)
            for (auto& v : bins)
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& e) { return gen_[size_t(e.id)] != e.gen; }),
                        v.end());
        stale_ = 0;
    }

    int w_ = -1, h_ = -1, bw_ = 0, bh_ = 0;
    std::vector<std::vector<std::vector<Entry>>> planes_;   // by plane: bins row-major, each its entries
    std::vector<uint32_t> gen_;     // by group id: registration generation, odd while listed
    std::vector<uint32_t> count_;   // by group id: entries of its live listing
    std::vector<uint32_t> seen_;    // by group id: the query that last gathered it
    std::vector<uint32_t> mark_;    // by bin: the listing that last wrote it (one entry per bin and listing)
    uint32_t markGen_ = 0, seenGen_ = 0;
    uint64_t live_ = 0, stale_ = 0;
    Stats stats_;
};

}  // namespace tak::sim::routebins
