#pragma once
// legion_route.h -- the route-query interface of Legion's navigator
// (docs/legion-pathfinding.md "Route queries").
//
// Movement consumers -- steering and its descent, the pinwheel and the passage gate, group awareness, destination
// packing (formation and packed slots, the area bounds), attack rings, the settle rule, the lift walk -- read a route
// through RouteView only. RouteGrid is the storage behind it: one integer window per route, owned and written by the
// route builder (legion.cpp: startField, advance, serve), stored in sparse tiles since stage 6 (legion_hier.h's
// sectors); nothing outside the builder and this header reads RouteGrid's cells.
//
// What a query can answer is kept apart:
//  * an estimate (direction guidance): exact where settled; on a build still running, where the frontier reached a
//    cell but has not settled it, the length of a real (not necessarily shortest) way; kNone where nothing is known.
//    A descent on estimates strictly decreases and ends at a seed, so it steers soundly on a half-built route;
//  * a known distance: exact, or kNone -- never a tentative value;
//  * reachability: Reachable (some finite estimate: a real way exists), Unreachable (proven: a complete route that
//    spans the cell's whole static component, and that no static change has reached since its build began, does not
//    reach it), Unknown (anything else: not computed yet, outside a bounded window, or touched by a later change --
//    whose group is then stale and refreshing). Unknown never retires an order;
//  * coverage: a cell settled with a margin of key to spare, i.e. every cell whose key is within the margin of it
//    is final (A* with a consistent heuristic settles keys in order). A consumer states the region it needs as
//    (cell, margin); see RouteView::covered;
//  * progress: a distance together with the metric it is measured in (the route's serial). Progress compares only
//    within one metric; a new metric re-baselines and never counts as a gain;
//  * freshness: the geometry epoch the route was built on, and whether a later change touched it, apart from all of
//    the above.
#include "sim/copymember.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tak::sim {

namespace route {
// Field potentials are 32-bit; kNone is "nothing known / no route". (At 16 bits they saturated at 65535, about 13,100
// orthogonal steps: serplong.scn's 30-lane serpentine reads 87,356 at its head.)
inline constexpr uint32_t kNone = 0xffffffff;
// Step costs: 7/5 = 1.4 ~ sqrt(2). The heuristic is the octile distance in these units.
inline constexpr uint16_t kOrthogonal = 5, kDiagonal = 7;
}  // namespace route

enum class Reach : uint8_t { Unknown, Reachable, Unreachable };

// A distance still to go, in a stated metric. Compare values only when the metrics are equal; a new metric
// re-baselines a progress measure and never counts as a gain (nor restarts a stall). The value is route-wide: a
// distance to the route's goals, never a distance to an intermediate boundary (a provider built from tiles adds the
// exit's route-wide remainder and gives the tile chain its own metric), so 0 is a goal of the route and never a tile
// exit. (The navigator's members read 0 as standing on one of the route's seeds, which is not necessarily their own
// goal: they measure the rest in squared cells to it, a metric of its own.)
struct Progress {
    uint32_t value = 0;
    uint64_t metric = 0;
    bool operator==(const Progress&) const = default;
};

// The route storage: an integer distance window over the map, built from the route's goals (seeds) by A* toward
// the group's bodies (aimed: target tx,tz) with a consistent octile heuristic, run to exhaustion (done) or paused once
// it covers its subscribers (covered). Written only by the builder.
//
// Sparse since stage 6: the window is covered by K x K tiles (K = 1 << shift) aligned to the map's sector grid
// (legion_hier.h), and a tile is allocated the first time the builder writes a cell in it; a tile never written
// reads kNone everywhere. So a build that paused, or that the frontier never carried across most of its window, holds
// only the tiles it reached, and a route's storage no longer has to be one rectangle (stage 6's corridors and tile
// chains: a tile may also be another route's, read with a per-tile offset).
struct RouteGrid {
    int W = 0, x0 = 0, z0 = 0, fw = 0, fh = 0;   // map width; window origin and size (cells)
    int shift = 5, sx0 = 0, sz0 = 0, sw = 0, sh = 0;   // tile size 1 << shift; the window's tile box (sector coords)
    std::vector<const uint32_t*> tiles;          // sw*sh: each tile's K*K potentials (row-major), nullptr: none yet
    LeftEmptyOnCopy<std::vector<std::unique_ptr<uint32_t[]>>> own;   // sw*sh: the tiles this route allocated (Impl::copyField)
    size_t ownTiles = 0;                         // tiles allocated so far
    // A corridor (stage 6): the tiles of the window the route may cover (1) or not (0); empty: every tile. A cell
    // of a tile outside the corridor is outside the route (inside() false), so it is never written and reads kNone.
    std::vector<uint8_t> allow;
    // A tile chain (stage 6): a tile may be shared with other routes and read with this route's offset (the rest of
    // the way from its exit, route-wide: Progress stays a distance to the goals), and a tile attached whole is final
    // everywhere it reaches (fin), whatever the build's key. Empty: no offsets, no attached tiles.
    std::vector<int64_t> offset;
    std::vector<uint8_t> fin;
    // The hot reads go through these (bind(): nullptr for an empty vector), so a window route's lookup is the tile
    // load alone. Call bind() after allow / offset / fin are sized (initTiles does; their sizes never change after).
    const uint8_t* allowP = nullptr;
    const int64_t* offsetP = nullptr;
    const uint8_t* finP = nullptr;
    void bind() {
        allowP = allow.empty() ? nullptr : allow.data();
        offsetP = offset.empty() ? nullptr : offset.data();
        finP = fin.empty() ? nullptr : fin.data();
    }
    uint32_t current = 0;                         // the build's current key (every key below it is settled)
    int tx = 0, tz = 0;                           // heuristic target
    bool aimed = false;
    bool done = false;                            // every reachable cell of the window settled
    // Paused (T7 C1): a first build that stopped once its subscribers were covered; its frontier is kept and it
    // resumes on demand. Never cleared: a resumed build only settles more cells.
    bool covered = false;
    // The window is a margin round the group's bodies and seeds, not its whole static component.
    bool bounded = false;
    bool softened = false;                        // some step's cost was raised by a soft obstacle
    uint64_t serial = 0;                          // unique per route built (the metric of its Progress)
    uint64_t epoch = 0;                           // the static geometry epoch it was built on
    // A static change reached this window after the build began (legion.cpp staticChanged: the changes that touch
    // the window's box, or every change made without boxes). Not a comparison of epochs: a change on another island
    // or another movement plane bumps the shared epoch without touching this window, and the route stays exact --
    // nothing schedules its refresh, so an epoch test would leave "unreached" unproven forever. Never hashed: a
    // function of the hashed epochs, windows and the world's static changes.
    bool touched = false;
    uint64_t work = 0;                            // relaxations so far: changes whenever an estimate may change

    // Size the window and its (empty) tile table. Every cell reads kNone until written.
    void initTiles(int mapW, int wx0, int wz0, int wfw, int wfh, int tileShift) {
        W = mapW; x0 = wx0; z0 = wz0; fw = wfw; fh = wfh; shift = tileShift;
        sx0 = x0 >> shift; sz0 = z0 >> shift;
        sw = fw > 0 ? ((x0 + fw - 1) >> shift) - sx0 + 1 : 0;
        sh = fh > 0 ? ((z0 + fh - 1) >> shift) - sz0 + 1 : 0;
        tiles.assign(size_t(sw) * size_t(sh), nullptr);
        own.clear();
        own.resize(size_t(sw) * size_t(sh));
        ownTiles = 0;
        allow.clear();
        offset.clear();
        fin.clear();
        bind();
    }
    size_t cells() const { return size_t(fw) * size_t(fh); }   // the window's cells
    // shift is 2..8; the mask bounds the size for GCC 13/14's -Walloc-size-larger-than.
    size_t tileCells() const { return size_t(1) << (2 * (shift & 15)); }
    bool inside(int x, int z) const {
        return x >= x0 && z >= z0 && x < x0 + fw && z < z0 + fh && (!allowP || allowP[tileOf(x, z)]);
    }
    size_t tileOf(int x, int z) const { return size_t(((z >> shift) - sz0) * sw + ((x >> shift) - sx0)); }
    size_t inTile(int x, int z) const {
        const int m = (1 << shift) - 1;
        return size_t(((z & m) << shift) | (x & m));
    }
    uint32_t cellAt(int x, int z) const { return inside(x, z) ? cellIn(x, z) : route::kNone; }
    // cellAt of a cell known to be inside() (the builder's relaxation has just tested it).
    uint32_t cellIn(int x, int z) const {
        const size_t ti = tileOf(x, z);
        const uint32_t* t = tiles[ti];
        if (!t) return route::kNone;
        const uint32_t v = t[inTile(x, z)];
        if (!offsetP || v == route::kNone) return v;
        return uint32_t(int64_t(v) + offsetP[ti]);   // a chain keeps every sum below kNone (legion.cpp attachTile)
    }
    // An attached tile: every value it holds is final.
    bool finalTile(int x, int z) const { return finP && inside(x, z) && finP[tileOf(x, z)]; }
    // The builder's write: the cell's slot, its tile allocated (all kNone) on first use. (x, z) inside the window.
    uint32_t& slot(int x, int z) {
        const size_t t = tileOf(x, z);
        if (!own[t]) {
            own[t].reset(new uint32_t[tileCells()]);
            std::fill(own[t].get(), own[t].get() + tileCells(), route::kNone);
            tiles[t] = own[t].get();
            ++ownTiles;
        }
        return own[t][inTile(x, z)];
    }
    // Release the storage (a finished build's tiles stay; this is for tests and resets).
    void clearTiles() { tiles.assign(tiles.size(), nullptr); own.clear(); own.resize(tiles.size()); ownTiles = 0; }
    uint32_t heuristic(int x, int z) const {
        if (!aimed) return 0;
        const int dx = x > tx ? x - tx : tx - x, dz = z > tz ? z - tz : tz - z;
        const int lo = dx < dz ? dx : dz, hi = dx < dz ? dz : dx;
        return uint32_t(route::kOrthogonal * hi + (route::kDiagonal - route::kOrthogonal) * lo);
    }
    // Is estimate v of cell final? (A done build: every reached cell is.)
    bool isFinal(size_t cell, uint32_t v) const {
        if (done) return v != route::kNone;
        if (v != route::kNone && finalTile(int(cell % size_t(W)), int(cell / size_t(W)))) return true;
        return v != route::kNone && uint32_t(v) + heuristic(int(cell % size_t(W)), int(cell / size_t(W))) <= current;
    }
    // Settled with `margin` of key to spare (a done build: reached at all).
    bool ahead(size_t cell, uint32_t margin) const {
        const uint32_t v = cellAt(int(cell % size_t(W)), int(cell / size_t(W)));
        if (done) return v != route::kNone;
        if (v != route::kNone && finalTile(int(cell % size_t(W)), int(cell / size_t(W)))) return true;
        return v != route::kNone &&
               uint32_t(v) + heuristic(int(cell % size_t(W)), int(cell / size_t(W))) + margin <= current;
    }
};

// A read-only view of one route. Cheap to copy (one pointer); every query is inline.
class RouteView {
public:
    // Implicit from the storage: a function that takes a RouteView can be handed a route as it is held.
    RouteView() = default;
    RouteView(const RouteGrid* g) : g_(g) {}
    RouteView(const RouteGrid& g) : g_(&g) {}
    explicit operator bool() const { return g_ != nullptr; }
    bool operator==(const RouteView& o) const { return g_ == o.g_; }

    // ---- identity and freshness ----------------------------------------------------------------------------------
    uint64_t metric() const { return g_->serial; }         // Progress::metric of this route
    uint64_t geometryEpoch() const { return g_->epoch; }   // freshness: the static epoch it was built on
    uint64_t version() const { return g_->work; }          // any estimate may differ when this differs
    // ---- build state (false on an empty view) ---------------------------------------------------------------------
    bool complete() const { return g_ && g_->done; }                      // every reachable cell settled
    bool paused() const { return g_ && g_->covered && !g_->done; }        // a paused build (C1)
    bool usable() const { return g_ && (g_->done || g_->covered); }       // complete, or paused, its subscribers covered
    bool bounded() const { return g_->bounded; }    // the window does not span the static component
    bool softened() const { return g_->softened; }  // some step's cost was raised by a soft obstacle
    bool inside(int x, int z) const { return g_->inside(x, z); }

    // ---- distance ------------------------------------------------------------------------------------------------
    // Direction guidance: exact where settled, an upper bound (a real way's length) where reached but unsettled,
    // route::kNone where nothing is known. Use known() where a value must be final.
    uint32_t estimate(int x, int z) const { return g_->cellAt(x, z); }
    uint32_t estimate(size_t cell) const { return g_->cellAt(int(cell % size_t(g_->W)), int(cell / size_t(g_->W))); }
    bool settled(size_t cell) const { return g_->isFinal(cell, estimate(cell)); }
    // The exact distance where settled, route::kNone elsewhere (never a tentative value).
    uint32_t known(size_t cell) const {
        const uint32_t v = estimate(cell);
        return g_->isFinal(cell, v) ? v : route::kNone;
    }
    // Coverage of a required neighbourhood: the cell is settled with `margin` of key to spare. Every cell whose key
    // (distance + heuristic) is at most the cell's key plus the margin is then final; for a cell c within path cost
    // d of this one, key(c) <= key(this) + d + (h(c) - h(this)) <= key(this) + 2d, so margin 2d covers every cell
    // within cost d. A complete route covers every reached cell.
    bool covered(size_t cell, uint32_t margin) const { return g_->ahead(cell, margin); }
    // Reachability knowledge (see the header comment). A touched route is built on older geometry around it; its
    // group is stale and refreshes it (staticChanged), and the refresh, built on the newer geometry, decides.
    Reach reach(size_t cell) const {
        if (estimate(cell) != route::kNone) return Reach::Reachable;
        if (g_->done && !g_->bounded && !g_->touched) return Reach::Unreachable;
        return Reach::Unknown;
    }
    // Progress still to go from a settled cell, in this route's metric; nullopt where it is not settled.
    std::optional<Progress> progress(size_t cell) const {
        const uint32_t v = known(cell);
        if (v == route::kNone) return std::nullopt;
        return Progress{v, g_->serial};
    }

private:
    const RouteGrid* g_ = nullptr;
};

// The demand on a paused build (C1): the neighbourhoods its subscribers need settled, each a (cell, margin) --
// the cell settled with that much key to spare (RouteView::covered). A subscriber is a member standing where the
// build has not covered it (and, from stage 5, a consumer's stated region). One entry per cell, the widest margin
// kept, in arrival order (the order the checksum folds them in); the builder drops the entries it has met. No cap:
// every need is represented until it is met. (Until stage 5 the list held the newest 32 cells and dropped the
// oldest: past 32 standing members the build never saw some members' needs, and they stood on a paused build
// that never resumed for them.) Lookup by cell is indexed, so a need costs O(log n), not a walk of the list.
class RouteDemand {
public:
    bool empty() const { return e_.empty(); }
    size_t size() const { return e_.size(); }
    const std::vector<std::pair<int, uint32_t>>& entries() const { return e_; }
    // Add a need; an existing entry for the cell keeps the wider margin. True when the cell is new.
    bool need(int cell, uint32_t margin) {
        const auto [at, fresh] = index_.try_emplace(cell, e_.size());
        if (!fresh) {
            auto& m = e_[at->second].second;
            if (margin > m) m = margin;
            return false;
        }
        e_.push_back({cell, margin});
        return true;
    }
    // Drop every entry `met(cell, margin)` says is met, keeping the others in order.
    template <class Met> void drop(const Met& met) {
        size_t out = 0;
        bool dropped = false;
        for (size_t i = 0; i < e_.size(); ++i) {
            if (met(e_[i].first, e_[i].second)) { dropped = true; continue; }
            e_[out++] = e_[i];
        }
        if (!dropped) return;
        e_.resize(out);
        index_.clear();
        for (size_t i = 0; i < e_.size(); ++i) index_.emplace(e_[i].first, i);
    }
    void clear() {
        std::vector<std::pair<int, uint32_t>>().swap(e_);
        index_.clear();
    }

private:
    std::vector<std::pair<int, uint32_t>> e_;
    std::map<int, size_t> index_;   // cell -> position in e_
};

}  // namespace tak::sim
