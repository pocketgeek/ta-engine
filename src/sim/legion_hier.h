#pragma once
// legion_hier.h -- the routing hierarchy above Legion's exact footprint-origin planes
// (docs/legion-pathfinding.md "Hierarchical routes and tiles").
//
// A plane (one mobility class's `legal` origins) is cut into square SECTORS of K x K origins, K a power of two
// (32; the size was chosen by measurement, see the record). Inside a sector the legal origins fall into REGIONS:
// the connected parts of the sector under the mover's step rule. That rule is 8-connected with no corner cutting
// (a diagonal step needs both orthogonal neighbours legal), and a diagonal step's two orthogonal cells lie in the
// rectangle its ends span, so in the same sector: two legal origins of one sector are connected inside it exactly
// when they are 4-connected inside it. A sector is never collapsed into one node; every region is its own node.
//
// PORTALS join regions of neighbouring sectors: a maximal run of boundary rows (east side) or columns (south side)
// where the origins on both sides of the boundary are legal, i.e. an orthogonal step across it. A diagonal step
// across a boundary needs both of its orthogonal cells legal, and one of them is an orthogonal crossing of the same
// boundary from the same cell (or into the same cell), so the portals carry every legal connection: two origins
// are in one static component of the plane exactly when their regions are joined by a chain of portals
// (legion_hier_test, and connectivityMatches under TAK_LEGION_VERIFY). Clearance is the plane's: `legal` is the
// exact footprint-origin predicate (rectangle clearance by run lengths), so nothing here approximates a footprint
// by a radius, and the diagonal / halo rules are the step rule's own.
//
// Identities are generation-aware (docs 3.4: PlaneId, SectorId, RegionId, PortalId). A sector rebuilt after a
// legality change gets a new generation from a counter that only grows for the plane id, and a plane slot reused
// for another mobility class gets a new PlaneId::generation, so a key made on older geometry or for another class
// never matches a newer one.
//
// Sectors are built on first use (ensure) and rebuilt on next use after invalidate(); the work is returned in cell
// units for the caller to charge (NavWork::Connectivity / RouteSearch). Everything here is a pure function of the
// plane's legality and the order of those calls; it is derived state and never hashed (the plane is not either).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace tak::sim::hier {

inline constexpr uint16_t kNoRegion = 0xffff;

struct PlaneId {
    uint16_t slot = 0;
    uint32_t generation = 0;   // a reused slot gets a new generation (never the old class's tiles)
    auto operator<=>(const PlaneId&) const = default;
};
struct SectorId {
    PlaneId plane;
    uint16_t sx = 0, sz = 0;
    auto operator<=>(const SectorId&) const = default;
};
struct RegionId {
    SectorId sector;
    uint16_t local = 0;
    uint32_t geometryGen = 0;   // the sector's generation when the region was labelled
    auto operator<=>(const RegionId&) const = default;
};
struct PortalId {
    RegionId a, b;   // a: the west / north side, b: the east / south side
    uint16_t index = 0;
    auto operator<=>(const PortalId&) const = default;
};

// Route-unit octile distance (5 per orthogonal, 7 per diagonal step; legion_route.h's costs).
inline uint32_t octile(int ax, int az, int bx, int bz) {
    const int dx = ax > bx ? ax - bx : bx - ax, dz = az > bz ? az - bz : bz - az;
    const int lo = dx < dz ? dx : dz, hi = dx < dz ? dz : dx;
    return uint32_t(5 * hi + 2 * lo);
}

// One maximal legal run across a sector boundary.
struct Portal {
    int a = 0, b = 0;           // sector a and its east (side 0) or south (side 1) neighbour b
    uint16_t ra = 0, rb = 0;    // the region on each side
    uint8_t side = 0;
    int start = 0, len = 0;     // the run: rows start..start+len-1 (east side) or columns (south side)
};

struct Region {
    int cells = 0;
    int rep = -1;                          // its first origin in row-major order (cell index)
    std::array<int, 4> box{0, 0, -1, -1};  // minX, minZ, maxX, maxZ
};

class Hierarchy {
public:
    struct Sector {
        uint32_t gen = 0;          // 0: never built
        bool dirty = true;         // regions to (re)label before use
        bool eastDirty = true, southDirty = true;
        std::vector<Region> regions;
        std::vector<Portal> east, south;
        std::vector<uint16_t> label;   // K x K: each origin's region (kNoRegion: illegal); allocated on first build
    };

    // (Re)start on a W x H plane with K = 1 << shift. Every sector is unbuilt.
    void reset(int W, int H, int shift, PlaneId id) {
        W_ = W; H_ = H; shift_ = shift; K_ = 1 << shift; id_ = id;
        SW_ = (W + K_ - 1) >> shift; SH_ = (H + K_ - 1) >> shift;
        sectors_.assign(size_t(SW_) * size_t(SH_), Sector{});
        built_ = 0;
    }
    bool ready() const { return W_ > 0; }
    PlaneId id() const { return id_; }
    void setId(PlaneId id) { id_ = id; }
    int K() const { return K_; }
    int shift() const { return shift_; }
    int width() const { return W_; }
    int height() const { return H_; }
    int sectorsWide() const { return SW_; }
    int sectorsHigh() const { return SH_; }
    int sectorCount() const { return SW_ * SH_; }
    int sectorOf(int x, int z) const { return (z >> shift_) * SW_ + (x >> shift_); }
    int sectorOfCell(int cell) const { return sectorOf(cell % W_, cell / W_); }
    // The sector's cell rectangle [x0, x1) x [z0, z1).
    std::array<int, 4> rect(int s) const {
        const int x0 = (s % SW_) << shift_, z0 = (s / SW_) << shift_;
        return {x0, z0, std::min(W_, x0 + K_), std::min(H_, z0 + K_)};
    }
    const Sector& sector(int s) const { return sectors_[size_t(s)]; }
    bool built(int s) const { return !sectors_[size_t(s)].dirty; }
    uint32_t gen(int s) const { return sectors_[size_t(s)].gen; }
    size_t builtSectors() const { return built_; }
    // The region of a legal origin (kNoRegion when illegal). Only meaningful in a built sector.
    uint16_t regionAt(int cell) const {
        const Sector& S = sectors_[size_t(sectorOfCell(cell))];
        if (S.label.empty()) return kNoRegion;
        return S.label[size_t(((cell / W_) & (K_ - 1)) << shift_ | ((cell % W_) & (K_ - 1)))];
    }
    RegionId regionId(int s, uint16_t r) const {
        return RegionId{SectorId{id_, uint16_t(s % SW_), uint16_t(s / SW_)}, r, sectors_[size_t(s)].gen};
    }

    // Label the sector's regions if it is unbuilt or invalidated. `work` gains the cells read.
    void ensure(int s, const uint8_t* legal, uint64_t& work) {
        if (sectors_[size_t(s)].dirty) build(s, legal, work);
    }
    // The portals on one side of sector s (0 east, 1 south), both sectors built.
    const std::vector<Portal>& side(int s, int which, const uint8_t* legal, uint64_t& work) {
        Sector& S = sectors_[size_t(s)];
        ensure(s, legal, work);
        const int n = which == 0 ? ((s % SW_) + 1 < SW_ ? s + 1 : -1) : ((s / SW_) + 1 < SH_ ? s + SW_ : -1);
        if (n < 0) { static const std::vector<Portal> none; return none; }
        ensure(n, legal, work);
        bool& dirty = which == 0 ? S.eastDirty : S.southDirty;
        if (dirty) buildSide(s, n, which, legal, work);
        return which == 0 ? S.east : S.south;
    }
    // Visit every portal of region r of sector s (all four sides): f(const Portal&, bool aSide, uint32_t handle),
    // aSide when s is the portal's `a`. Order: east, south, west neighbour's east, north neighbour's south.
    template <class F> void forPortals(int s, uint16_t r, const uint8_t* legal, uint64_t& work, F&& f) {
        {
            const auto& e = side(s, 0, legal, work);
            for (size_t i = 0; i < e.size(); ++i) if (e[i].ra == r) f(e[i], true, handle(s, 0, i));
        }
        {
            const auto& so = side(s, 1, legal, work);
            for (size_t i = 0; i < so.size(); ++i) if (so[i].ra == r) f(so[i], true, handle(s, 1, i));
        }
        if (s % SW_ > 0) {
            const auto& e = side(s - 1, 0, legal, work);
            for (size_t i = 0; i < e.size(); ++i) if (e[i].rb == r) f(e[i], false, handle(s - 1, 0, i));
        }
        if (s / SW_ > 0) {
            const auto& so = side(s - SW_, 1, legal, work);
            for (size_t i = 0; i < so.size(); ++i) if (so[i].rb == r) f(so[i], false, handle(s - SW_, 1, i));
        }
    }
    static uint32_t handle(int s, int which, size_t i) { return uint32_t(s) << 10 | uint32_t(which) << 9 | uint32_t(i); }
    // A portal by handle; valid until either of its sectors is rebuilt (callers hold handles within one search).
    const Portal& portal(uint32_t h) const {
        const Sector& S = sectors_[size_t(h >> 10)];
        return (h >> 9 & 1) ? S.south[h & 511] : S.east[h & 511];
    }
    PortalId portalId(uint32_t h) const {
        const Portal& p = portal(h);
        return PortalId{regionId(p.a, p.ra), regionId(p.b, p.rb), uint16_t(h & 511)};
    }
    // Pair i of portal p: the origin on a's side and the one on b's side.
    int nearCell(const Portal& p, int i) const {
        const auto r = rect(p.a);
        return p.side == 0 ? (p.start + i) * W_ + (r[2] - 1) : (r[3] - 1) * W_ + (p.start + i);
    }
    int farCell(const Portal& p, int i) const { return nearCell(p, i) + (p.side == 0 ? 1 : W_); }
    // The origin of `p` on the side of sector s (the a side when s == p.a), at the run's middle.
    int midCell(const Portal& p, int s) const { return s == p.a ? nearCell(p, p.len / 2) : farCell(p, p.len / 2); }

    // Legality changed for origins in box {x0, z0, x1, z1} (inclusive): the sectors it touches, grown by one
    // origin (a boundary origin is half of its neighbour's portal pairs), relabel on next use.
    void invalidate(const std::array<int, 4>& box) {
        if (!ready() || box[2] < box[0] || box[3] < box[1]) return;
        const int x0 = std::max(0, box[0] - 1) >> shift_, z0 = std::max(0, box[1] - 1) >> shift_;
        const int x1 = std::min(W_ - 1, box[2] + 1) >> shift_, z1 = std::min(H_ - 1, box[3] + 1) >> shift_;
        for (int sz = z0; sz <= z1; ++sz)
            for (int sx = x0; sx <= x1; ++sx) markDirty(sz * SW_ + sx);
    }
    void invalidateAll() {
        for (int s = 0; s < sectorCount(); ++s) markDirty(s);
    }

    // Deterministic bytes (NavBytes::portals): the sector records, each built sector's labels (2 bytes an
    // origin), its regions and portals at fixed record sizes. Platform-independent; observation only.
    uint64_t bytes() const {
        uint64_t b = uint64_t(sectors_.size()) * 64;
        for (const Sector& S : sectors_) b += uint64_t(S.label.size()) * 2 + uint64_t(S.regions.size()) * 28 + uint64_t(S.east.size() + S.south.size()) * 20;
        return b;
    }

    // ---- checks (tests, TAK_LEGION_VERIFY) ----------------------------------------------------------------------
    // A built sector equals a fresh labelling of the current plane, and its built sides fresh portal scans.
    bool sectorMatches(int s, const uint8_t* legal) const {
        const Sector& S = sectors_[size_t(s)];
        if (S.dirty) return true;
        Hierarchy fresh;
        fresh.W_ = W_; fresh.H_ = H_; fresh.shift_ = shift_; fresh.K_ = K_; fresh.SW_ = SW_; fresh.SH_ = SH_;
        fresh.sectors_.assign(sectors_.size(), Sector{});
        uint64_t w = 0;
        fresh.build(s, legal, w);
        const auto r = rect(s);
        for (int z = r[1]; z < r[3]; ++z)
            for (int x = r[0]; x < r[2]; ++x)
                if (fresh.regionAt(z * W_ + x) != regionAt(z * W_ + x)) return false;
        const Sector& F = fresh.sectors_[size_t(s)];
        if (F.regions.size() != S.regions.size()) return false;
        for (size_t i = 0; i < F.regions.size(); ++i)
            if (F.regions[i].cells != S.regions[i].cells || F.regions[i].rep != S.regions[i].rep ||
                F.regions[i].box != S.regions[i].box) return false;
        for (int which = 0; which < 2; ++which) {
            if (which == 0 ? S.eastDirty : S.southDirty) continue;
            const int n = which == 0 ? s + 1 : s + SW_;
            if (sectors_[size_t(n)].dirty) return false;   // a built side needs both sectors built
            fresh.build(n, legal, w);
            fresh.buildSide(s, n, which, legal, w);
            const auto& a = which == 0 ? S.east : S.south;
            const auto& b = which == 0 ? fresh.sectors_[size_t(s)].east : fresh.sectors_[size_t(s)].south;
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].ra != b[i].ra || a[i].rb != b[i].rb || a[i].start != b[i].start || a[i].len != b[i].len) return false;
        }
        return true;
    }
    // Build every sector and side, then: two legal origins share a plane component (`comp`, -1 illegal) exactly
    // when their regions are joined by portals. Whole-plane work: tests and small verify runs only.
    bool connectivityMatches(const uint8_t* legal, const int32_t* comp) {
        uint64_t w = 0;
        std::vector<int> base(sectors_.size() + 1, 0);
        for (int s = 0; s < sectorCount(); ++s) {
            ensure(s, legal, w);
            base[size_t(s) + 1] = base[size_t(s)] + int(sectors_[size_t(s)].regions.size());
        }
        std::vector<int> parent(size_t(base.back()));
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
        auto find = [&](int i) { while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])]; return i; };
        for (int s = 0; s < sectorCount(); ++s)
            for (int which = 0; which < 2; ++which)
                for (const Portal& p : side(s, which, legal, w)) {
                    const int a = find(base[size_t(p.a)] + p.ra), b = find(base[size_t(p.b)] + p.rb);
                    if (a != b) parent[size_t(std::max(a, b))] = std::min(a, b);
                }
        std::map<int, int> toComp, toRoot;
        for (int c = 0; c < W_ * H_; ++c) {
            const bool l = legal[size_t(c)] != 0;
            if (l != (regionAt(c) != kNoRegion) || l != (comp[c] >= 0)) return false;
            if (!l) continue;
            const int root = find(base[size_t(sectorOfCell(c))] + regionAt(c));
            if (toComp.try_emplace(root, comp[c]).first->second != comp[c]) return false;
            if (toRoot.try_emplace(comp[c], root).first->second != root) return false;
        }
        return true;
    }

    // ---- the abstract search ---------------------------------------------------------------------------------------
    // A tree of regions over which a route is built: every target origin's region linked toward the goals' regions.
    // Node: a region, its parent (-1 for a goal region) and the portal it leaves through toward the parent (the
    // parent is the portal's other side); `entry` is the origin the search entered it by (a goal origin, or the
    // portal midpoint on its side) and `cost` the search's estimate from there to the goals, in route units.
    struct Node {
        int s = 0;
        uint16_t r = 0;
        int parent = -1;
        uint32_t exit = ~0u;
        int entry = -1;
        uint64_t cost = 0;
    };
    struct Tree {
        std::vector<Node> nodes;
        std::map<uint64_t, int> index;   // key(s, r) -> node
        bool complete = false;           // every target's region reached
        size_t expanded = 0;
    };
    static uint64_t key(int s, uint16_t r) { return uint64_t(uint32_t(s)) << 16 | r; }
    // Dijkstra over regions from the goals' regions (cost 0 at the goal origin) until every target origin's
    // region is settled or `maxExpand` regions are expanded. The cost of a step from region X to its neighbour Y
    // across portal Q: the octile distance from X's entry to Q's midpoint on X's side, plus one orthogonal step
    // across; Y's entry is Q's midpoint on its own side. Ties: lower cost, then lower key (deterministic). The
    // tree holds each target's chain of predecessors; a chain stops at the first region already in the tree, so
    // every region has one parent and following parents always ends at a goal region (no cycles). `work` gains
    // the regions expanded and portals scanned (plus any sectors built on the way).
    Tree search(const std::vector<int>& goals, const std::vector<int>& targets, const uint8_t* legal, size_t maxExpand,
                uint64_t& work) {
        return search(goals, targets, legal, maxExpand, work, [](int, uint16_t, uint64_t) { return uint64_t(0); });
    }
    // The same with a traffic cost (Legion 10,000 stage 7): extra(s, r, step) is added to each step out of region
    // (s, r), `step` being that step's own cost (the way across the region from its entry to the portal). A pure
    // function of its arguments during one search; 0 everywhere gives the search above exactly. Stage 8: an extra
    // that also takes (const Portal& q, bool aSide) -- the portal the step crosses and whether (s, r) is its `a`
    // side -- can charge the crossing itself (a chokepoint's queue, toward the goals: the bodies walk the other way).
    template <class Extra>
    Tree search(const std::vector<int>& goals, const std::vector<int>& targets, const uint8_t* legal, size_t maxExpand,
                uint64_t& work, Extra&& extra) {
        Tree t;
        // Per-region search state in a pool, a sector's regions in one run (allocated when the search first meets
        // the sector, generation-stamped, so nothing is cleared between searches). A reference into the pool is
        // never held across a call that may add a sector.
        if (++stamp_ == 0) { std::fill(sStamp_.begin(), sStamp_.end(), 0u); stamp_ = 1; }
        if (sStamp_.size() != sectors_.size()) { sStamp_.assign(sectors_.size(), 0u); sBase_.assign(sectors_.size(), 0u); }
        pool_.clear();
        auto slot = [&](uint64_t k) -> Best& {
            const int s = int(k >> 16);
            if (sStamp_[size_t(s)] != stamp_) {
                sStamp_[size_t(s)] = stamp_;
                sBase_[size_t(s)] = uint32_t(pool_.size());
                pool_.resize(pool_.size() + sectors_[size_t(s)].regions.size());
            }
            return pool_[sBase_[size_t(s)] + (k & 0xffff)];
        };
        using Item = std::pair<uint64_t, uint64_t>;   // (cost, key)
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        for (const int g : goals) {
            if (g < 0 || g >= W_ * H_ || !legal[size_t(g)]) continue;
            const int s = sectorOfCell(g);
            ensure(s, legal, work);
            const uint16_t r = regionAt(g);
            if (r == kNoRegion) continue;
            const uint64_t k = key(s, r);
            Best& b = slot(k);
            if (b.seen) { if (g < b.entry) b.entry = g; continue; }   // the lowest goal origin of a region
            b = Best{0, g, ~0ull, ~0u, false, true};
            open.push({0, k});
        }
        std::vector<uint64_t> want;
        for (const int c : targets) {
            if (c < 0 || c >= W_ * H_ || !legal[size_t(c)]) continue;
            const int s = sectorOfCell(c);
            ensure(s, legal, work);
            if (regionAt(c) != kNoRegion) want.push_back(key(s, regionAt(c)));
        }
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        size_t left = want.size();
        auto wanted = [&](uint64_t k) { return std::binary_search(want.begin(), want.end(), k); };
        while (!open.empty() && left > 0 && t.expanded < maxExpand) {
            const auto [cost, k] = open.top();
            open.pop();
            Best& b = slot(k);
            if (b.done || cost != b.cost) continue;
            b.done = true;
            ++t.expanded;
            ++work;
            if (wanted(k)) --left;
            const int s = int(k >> 16);
            const uint16_t r = uint16_t(k & 0xffff);
            const int ex = b.entry % W_, ez = b.entry / W_;
            forPortals(s, r, legal, work, [&](const Portal& p, bool aSide, uint32_t h) {
                ++work;
                const int ns = aSide ? p.b : p.a;
                const uint16_t nr = aSide ? p.rb : p.ra;
                const int here = midCell(p, s), there = midCell(p, ns);
                const uint64_t step = octile(ex, ez, here % W_, here / W_) + 5;
                uint64_t more;
                if constexpr (std::is_invocable_v<Extra&, int, uint16_t, uint64_t, const Portal&, bool>)
                    more = extra(s, r, step, p, aSide);
                else
                    more = extra(s, r, step);
                const uint64_t c = cost + step + more;
                const uint64_t nk = key(ns, nr);
                Best& n = slot(nk);
                if (n.seen && (n.done || c > n.cost || (c == n.cost && k >= n.from))) return;
                n = Best{c, there, k, h, false, true};
                open.push({c, nk});
            });
        }
        t.complete = left == 0;
        // Each wanted region's chain toward the goals, in key order; a chain stops at a region already in the tree.
        for (const uint64_t k0 : want) {
            if (sStamp_[size_t(k0 >> 16)] != stamp_ || !slot(k0).seen || !slot(k0).done) continue;
            std::vector<uint64_t> chain;
            for (uint64_t k = k0; k != ~0ull && !t.index.count(k); k = slot(k).from) chain.push_back(k);
            // Add from the goal end so a parent always precedes its children.
            for (size_t i = chain.size(); i-- > 0;) {
                const uint64_t k = chain[i];
                const Best& b = slot(k);
                Node n;
                n.s = int(k >> 16);
                n.r = uint16_t(k & 0xffff);
                n.entry = b.entry;
                n.cost = b.cost;
                if (b.from != ~0ull) {
                    n.parent = t.index.at(b.from);
                    n.exit = b.via;
                }
                t.index.emplace(k, int(t.nodes.size()));
                t.nodes.push_back(n);
            }
        }
        return t;
    }

private:
    void markDirty(int s) {
        Sector& S = sectors_[size_t(s)];
        if (S.dirty) return;
        S.dirty = true;
        --built_;
        // Every side that reads its labels: its own, its west neighbour's east and its north neighbour's south.
        S.eastDirty = S.southDirty = true;
        if (s % SW_ > 0) sectors_[size_t(s - 1)].eastDirty = true;
        if (s / SW_ > 0) sectors_[size_t(s - SW_)].southDirty = true;
    }
    void build(int s, const uint8_t* legal, uint64_t& work) {
        Sector& S = sectors_[size_t(s)];
        const auto r = rect(s);
        S.regions.clear();
        S.label.assign(size_t(K_) * size_t(K_), kNoRegion);
        auto lab = [&](int x, int z) -> uint16_t& { return S.label[size_t((z - r[1]) << shift_ | (x - r[0]))]; };
        uint64_t cells = 0;
        for (int z = r[1]; z < r[3]; ++z)
            for (int x = r[0]; x < r[2]; ++x) {
                const size_t c0 = size_t(z) * W_ + x;
                ++cells;
                if (!legal[c0] || lab(x, z) != kNoRegion) continue;
                const uint16_t id = uint16_t(S.regions.size());
                Region reg;
                reg.rep = int(c0);
                reg.box = {x, z, x, z};
                queue_.assign(1, int(c0));
                lab(x, z) = id;
                for (size_t head = 0; head < queue_.size(); ++head) {
                    const int c = queue_[head], cx = c % W_, cz = c / W_;
                    ++reg.cells;
                    reg.box = {std::min(reg.box[0], cx), std::min(reg.box[1], cz), std::max(reg.box[2], cx), std::max(reg.box[3], cz)};
                    const int nb[4][2] = {{cx + 1, cz}, {cx - 1, cz}, {cx, cz + 1}, {cx, cz - 1}};
                    for (const auto& n : nb) {
                        if (n[0] < r[0] || n[1] < r[1] || n[0] >= r[2] || n[1] >= r[3]) continue;
                        const size_t nc = size_t(n[1]) * W_ + n[0];
                        if (!legal[nc] || lab(n[0], n[1]) != kNoRegion) continue;
                        lab(n[0], n[1]) = id;
                        queue_.push_back(int(nc));
                    }
                }
                cells += 4 * uint64_t(reg.cells);
                S.regions.push_back(reg);
            }
        work += cells;
        if (S.dirty) ++built_;
        S.dirty = false;
        S.gen = ++genCounter_;
        // Its sides and the sides of its west and north neighbours read its labels: rescan them on use.
        S.eastDirty = S.southDirty = true;
        if (s % SW_ > 0) sectors_[size_t(s - 1)].eastDirty = true;
        if (s / SW_ > 0) sectors_[size_t(s - SW_)].southDirty = true;
    }
    void buildSide(int s, int n, int which, const uint8_t* legal, uint64_t& work) {
        Sector& S = sectors_[size_t(s)];
        auto& list = which == 0 ? S.east : S.south;
        list.clear();
        const auto r = rect(s);
        const int lo = which == 0 ? r[1] : r[0], hi = which == 0 ? r[3] : r[2];
        auto nearAt = [&](int t) { return which == 0 ? t * W_ + (r[2] - 1) : (r[3] - 1) * W_ + t; };
        const int step = which == 0 ? 1 : W_;
        for (int t = lo; t < hi; ++t) {
            ++work;
            const int a = nearAt(t), b = a + step;
            if (!legal[size_t(a)] || !legal[size_t(b)]) continue;
            Portal* last = list.empty() ? nullptr : &list.back();
            if (last && last->start + last->len == t) { ++last->len; continue; }
            Portal p;
            p.a = s; p.b = n; p.side = uint8_t(which);
            p.ra = regionAt(a); p.rb = regionAt(b);
            p.start = t; p.len = 1;
            list.push_back(p);
        }
        (which == 0 ? S.eastDirty : S.southDirty) = false;
    }

    // The search's scratch (never state): a region's best entry so far.
    struct Best { uint64_t cost = 0; int entry = -1; uint64_t from = ~0ull; uint32_t via = ~0u; bool done = false, seen = false; };
    std::vector<Best> pool_;
    std::vector<uint32_t> sStamp_, sBase_;
    uint32_t stamp_ = 0;
    int W_ = 0, H_ = 0, shift_ = 5, K_ = 32, SW_ = 0, SH_ = 0;
    PlaneId id_;
    std::vector<Sector> sectors_;
    std::vector<int> queue_;
    uint32_t genCounter_ = 0;
    size_t built_ = 0;
};

}  // namespace tak::sim::hier
