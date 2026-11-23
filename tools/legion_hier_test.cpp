// legion_hier_test -- the routing hierarchy (src/sim/legion_hier.h, Legion 10,000 stage 6): regions, portals,
// generations and the abstract search on synthetic planes, against brute-force labellings under the mover's step
// rule (8-connected, a diagonal step needs both orthogonal neighbours legal).
//
// Checked: two legal origins share a component exactly when their regions are joined by portals (random planes of
// several densities, map sizes that are not multiples of K, K = 8, 16, 32); diagonal-only contacts across a sector
// corner join nothing (the step rule forbids them); a sector rebuilt after invalidate() equals a fresh labelling
// and has a new generation; a reused plane id gives new region ids; the search's tree reaches every target in the
// goals' component, follows parents to a goal region without cycles, and leaves a target in another component
// unreached (incomplete), never "proven" by a missing node.
#include "sim/legion_hier.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace tak::sim;

namespace {
int failures = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::fprintf(stderr, "legion_hier_test:%d: FAILED %s\n", __LINE__, #c); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

uint64_t rng = 0x9e3779b97f4a7c15ull;
uint32_t next() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return uint32_t(rng >> 11);
}

struct Plane {
    int W = 0, H = 0;
    std::vector<uint8_t> legal;
    bool at(int x, int z) const { return x >= 0 && z >= 0 && x < W && z < H && legal[size_t(z) * W + x]; }
};

// Components under the step rule (the field's and the mover's), by flood fill.
std::vector<int32_t> components(const Plane& p) {
    std::vector<int32_t> comp(p.legal.size(), -1);
    int next = 0;
    std::vector<int> q;
    const int d[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (int c0 = 0; c0 < p.W * p.H; ++c0) {
        if (!p.legal[size_t(c0)] || comp[size_t(c0)] >= 0) continue;
        comp[size_t(c0)] = next;
        q.assign(1, c0);
        for (size_t h = 0; h < q.size(); ++h) {
            const int x = q[h] % p.W, z = q[h] / p.W;
            for (const auto& s : d) {
                const int nx = x + s[0], nz = z + s[1];
                if (!p.at(nx, nz)) continue;
                if (s[0] && s[1] && !(p.at(x + s[0], z) && p.at(x, z + s[1]))) continue;
                const size_t nc = size_t(nz) * p.W + nx;
                if (comp[nc] >= 0) continue;
                comp[nc] = next;
                q.push_back(int(nc));
            }
        }
        ++next;
    }
    return comp;
}

Plane randomPlane(int W, int H, int wallPermille, int blobs) {
    Plane p;
    p.W = W; p.H = H;
    p.legal.assign(size_t(W) * H, 1);
    for (auto& c : p.legal) c = (next() % 1000) >= uint32_t(wallPermille);
    for (int b = 0; b < blobs; ++b) {   // walls and blocks, so regions split inside sectors
        const int x = int(next() % uint32_t(W)), z = int(next() % uint32_t(H));
        const int len = 4 + int(next() % 40);
        const bool horiz = next() & 1;
        for (int i = 0; i < len; ++i) {
            const int cx = horiz ? x + i : x, cz = horiz ? z : z + i;
            if (cx < W && cz < H) p.legal[size_t(cz) * W + cx] = 0;
        }
    }
    return p;
}

void checkPlane(const Plane& p, int shift, const char* what) {
    hier::Hierarchy h;
    h.reset(p.W, p.H, shift, hier::PlaneId{3, 7});
    const auto comp = components(p);
    const bool ok = h.connectivityMatches(p.legal.data(), comp.data());
    if (!ok) std::fprintf(stderr, "  plane %s %dx%d K=%d\n", what, p.W, p.H, 1 << shift);
    CHECK(ok);
    // Portal pairs are legal, on the boundary, and joined by an orthogonal step.
    uint64_t w = 0;
    for (int s = 0; s < h.sectorCount(); ++s)
        for (int which = 0; which < 2; ++which)
            for (const auto& pt : h.side(s, which, p.legal.data(), w))
                for (int i = 0; i < pt.len; ++i) {
                    const int a = h.nearCell(pt, i), b = h.farCell(pt, i);
                    CHECK(p.legal[size_t(a)] && p.legal[size_t(b)]);
                    CHECK(h.sectorOfCell(a) == pt.a && h.sectorOfCell(b) == pt.b);
                    CHECK(h.regionAt(a) == pt.ra && h.regionAt(b) == pt.rb);
                }
}
}  // namespace

int main() {
    // 1. Connectivity equals the step rule's components on random planes.
    for (int round = 0; round < 60; ++round) {
        const int W = 17 + int(next() % 140), H = 17 + int(next() % 140);
        const int density = int(next() % 450);
        const Plane p = randomPlane(W, H, density, int(next() % 30));
        for (int shift : {3, 4, 5}) checkPlane(p, shift, "random");
    }
    // 2. Diagonal-only contact across a sector corner joins nothing: two legal blocks touching only at a corner
    // whose orthogonal cells are walls (K = 8: the corner of sectors (0,0) and (1,1) at (7,7)/(8,8)).
    {
        Plane p;
        p.W = 16; p.H = 16;
        p.legal.assign(256, 0);
        for (int z = 0; z < 8; ++z) for (int x = 0; x < 8; ++x) p.legal[size_t(z) * 16 + x] = 1;
        for (int z = 8; z < 16; ++z) for (int x = 8; x < 16; ++x) p.legal[size_t(z) * 16 + x] = 1;
        checkPlane(p, 3, "corner");
        const auto comp = components(p);
        CHECK(comp[7 * 16 + 7] != comp[8 * 16 + 8]);
        // Open one orthogonal cell: the step rule still forbids the diagonal (it needs BOTH), but (8,7) now joins
        // (7,7) and (8,8) by orthogonal steps, so the halves are one component and the portals must say so.
        p.legal[7 * 16 + 8] = 1;
        checkPlane(p, 3, "corner-open");
        const auto comp2 = components(p);
        CHECK(comp2[7 * 16 + 7] == comp2[8 * 16 + 8]);
    }
    // 3. Incremental: invalidate() after random changes; rebuilt sectors equal fresh ones, get new generations, and
    // connectivity still matches.
    for (int round = 0; round < 20; ++round) {
        Plane p = randomPlane(40 + int(next() % 90), 40 + int(next() % 90), 150, 10);
        hier::Hierarchy h;
        h.reset(p.W, p.H, 4, hier::PlaneId{1, 1});
        auto comp = components(p);
        CHECK(h.connectivityMatches(p.legal.data(), comp.data()));
        std::vector<uint32_t> gens(size_t(h.sectorCount()));
        for (int s = 0; s < h.sectorCount(); ++s) gens[size_t(s)] = h.gen(s);
        for (int edit = 0; edit < 8; ++edit) {
            const int x0 = int(next() % uint32_t(p.W)), z0 = int(next() % uint32_t(p.H));
            const int x1 = std::min(p.W - 1, x0 + int(next() % 6)), z1 = std::min(p.H - 1, z0 + int(next() % 6));
            const uint8_t v = next() & 1;
            for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) p.legal[size_t(z) * p.W + x] = v;
            h.invalidate({x0, z0, x1, z1});
            const int s = h.sectorOf(x0, z0);
            CHECK(!h.built(s));
        }
        uint64_t w = 0;
        for (int s = 0; s < h.sectorCount(); ++s) {
            const bool was = h.built(s);
            h.ensure(s, p.legal.data(), w);
            if (!was) CHECK(h.gen(s) != gens[size_t(s)]);
            CHECK(h.sectorMatches(s, p.legal.data()));
        }
        comp = components(p);
        CHECK(h.connectivityMatches(p.legal.data(), comp.data()));
        for (int s = 0; s < h.sectorCount(); ++s) CHECK(h.sectorMatches(s, p.legal.data()));
    }
    // 4. Identities: a reused plane slot (new generation) never gives an old region id.
    {
        const Plane p = randomPlane(64, 64, 100, 5);
        hier::Hierarchy h;
        uint64_t w = 0;
        h.reset(64, 64, 4, hier::PlaneId{2, 1});
        h.ensure(0, p.legal.data(), w);
        const auto a = h.regionId(0, 0);
        h.reset(64, 64, 4, hier::PlaneId{2, 2});   // the slot now holds another mobility class
        h.ensure(0, p.legal.data(), w);
        const auto b = h.regionId(0, 0);
        CHECK(!(a == b));
        CHECK(a.sector.plane.slot == b.sector.plane.slot && a.sector.plane.generation != b.sector.plane.generation);
    }
    // 5. The search: every target in the goals' component is in the tree; parents end at a goal region; a target
    // in another component leaves the tree incomplete.
    for (int round = 0; round < 30; ++round) {
        const Plane p = randomPlane(60 + int(next() % 100), 60 + int(next() % 100), 120 + int(next() % 200), 20);
        const auto comp = components(p);
        hier::Hierarchy h;
        h.reset(p.W, p.H, 4, hier::PlaneId{0, 1});
        std::vector<int> legalCells;
        for (int c = 0; c < p.W * p.H; ++c) if (p.legal[size_t(c)]) legalCells.push_back(c);
        if (legalCells.size() < 10) continue;
        const int goal = legalCells[next() % legalCells.size()];
        std::vector<int> targets;
        bool other = false;
        for (int i = 0; i < 12; ++i) {
            const int t = legalCells[next() % legalCells.size()];
            targets.push_back(t);
            other |= comp[size_t(t)] != comp[size_t(goal)];
        }
        uint64_t w = 0;
        const auto tree = h.search({goal}, targets, p.legal.data(), 1u << 20, w);
        CHECK(tree.complete == !other);
        for (const int t : targets) {
            const int s = h.sectorOfCell(t);
            const auto found = tree.index.find(hier::Hierarchy::key(s, h.regionAt(t)));
            if (comp[size_t(t)] != comp[size_t(goal)]) { CHECK(found == tree.index.end()); continue; }
            CHECK(found != tree.index.end());
            if (found == tree.index.end()) continue;
            int n = found->second, steps = 0;
            while (tree.nodes[size_t(n)].parent >= 0 && steps <= int(tree.nodes.size())) {
                const auto& node = tree.nodes[size_t(n)];
                const auto& par = tree.nodes[size_t(node.parent)];
                CHECK(node.parent < n);   // parents precede children
                // The exit portal joins the node's region and its parent's.
                const auto& pt = h.portal(node.exit);
                const bool joins = (pt.a == node.s && pt.ra == node.r && pt.b == par.s && pt.rb == par.r) ||
                                   (pt.b == node.s && pt.rb == node.r && pt.a == par.s && pt.ra == par.r);
                CHECK(joins);
                n = node.parent;
                ++steps;
            }
            CHECK(steps <= int(tree.nodes.size()));
            const auto& root = tree.nodes[size_t(n)];
            CHECK(root.s == h.sectorOfCell(goal) && root.r == h.regionAt(goal));
        }
    }
    if (failures) { std::fprintf(stderr, "legion_hier_test: %d failures\n", failures); return 1; }
    std::printf("legion_hier_test: ok\n");
    return 0;
}
