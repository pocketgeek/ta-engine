// legion_routebins_test -- the route invalidation index (src/sim/legion_routebins.h, perf follow-up I): for random
// routes (windows, corridors and chains' tile lists, empty and map-edge windows, other tile sizes) listed, re-listed and
// unlisted at random on several planes and map sizes, every group whose route a random change box reaches (touches(),
// legion.cpp's fieldTouched) is among the index's candidates for that box; every candidate is a group listed on that
// plane now; the read-only collect() (TAK_LEGION_VERIFY's query) finds the same groups and changes nothing; the bins
// hold exactly what a fresh listing would; and the live / stale counts agree through query-time drops and whole-index
// compactions.
#include "sim/legion_routebins.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

using namespace tak::sim;

namespace {
int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::fprintf(stderr, "legion_routebins_test:%d: FAILED %s\n", __LINE__, #c); \
            if (++failures > 20) std::exit(1);                                       \
        }                                                                            \
    } while (0)

struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    int below(int n) { return n > 0 ? int(next() % uint64_t(n)) : 0; }
    int range(int a, int b) { return a + below(b - a + 1); }
};

// A random route on a W x H map: a window (sometimes empty, sometimes at an edge) and, half the time, a corridor's
// tile list (random tiles of the window allowed, tile size 4..64).
std::unique_ptr<RouteGrid> randomRoute(Rng& r, int W, int H) {
    auto f = std::make_unique<RouteGrid>();
    int x0 = r.below(W), z0 = r.below(H);
    int fw = 1 + r.below(r.below(4) == 0 ? W : std::max(1, W / 4)), fh = 1 + r.below(r.below(4) == 0 ? H : std::max(1, H / 4));
    if (r.below(10) == 0) x0 = 0;
    if (r.below(10) == 0) z0 = 0;
    fw = std::min(fw, W - x0);
    fh = std::min(fh, H - z0);
    if (r.below(40) == 0) fw = 0;   // a window of no cells (never built, but touches() reads its ring)
    const int shift = r.range(2, 6);
    f->initTiles(W, x0, z0, fw, fh, shift);
    if (r.below(2) && f->sw > 0 && f->sh > 0) {
        f->allow.assign(size_t(f->sw) * size_t(f->sh), 0);
        const int density = r.range(1, 4);   // 1 in density tiles allowed (density 1: all)
        for (auto& a : f->allow) a = r.below(density) == 0;
        f->bind();
    }
    return f;
}

std::array<int, 4> randomBox(Rng& r, int W, int H) {
    switch (r.below(8)) {
    case 0: return {0, 0, W - 1, H - 1};   // the whole map (first epoch)
    case 1: {   // a cell on an edge
        const int x = r.below(2) ? 0 : W - 1, z = r.below(H);
        return {x, z, x, z};
    }
    default: {
        const int x0 = r.below(W), z0 = r.below(H);
        const int x1 = std::min(W - 1, x0 + r.below(r.below(6) == 0 ? 80 : 6)), z1 = std::min(H - 1, z0 + r.below(r.below(6) == 0 ? 80 : 6));
        return {x0, z0, x1, z1};
    }
    }
}

struct Group {
    int plane = -1;
    std::unique_ptr<RouteGrid> a, b;
};

void run(uint64_t seed, int W, int H, int planes, int steps) {
    Rng r{seed * 2654435761u + 12345};
    routebins::Index index;
    CHECK(index.size(W, H));
    CHECK(!index.size(W, H));
    std::map<int, Group> groups;   // listed groups by id
    int nextId = 1;
    auto relist = [&](int id, Group& g) {
        index.list(id, g.plane, g.a.get(), g.b.get());
        CHECK(index.listed(id) == (g.plane >= 0 && (g.a || g.b)));
    };
    for (int step = 0; step < steps; ++step) {
        const int op = r.below(10);
        if (op < 4 || groups.empty()) {   // a new group with a field (and sometimes a refresh building)
            Group g;
            g.plane = r.below(planes);
            g.a = randomRoute(r, W, H);
            if (r.below(3) == 0) g.b = randomRoute(r, W, H);
            const int id = nextId++;
            relist(id, g);
            groups.emplace(id, std::move(g));
        } else if (op < 7) {   // a field replaced (refresh installed, shared, widened) or dropped (eviction)
            auto it = groups.begin();
            std::advance(it, r.below(int(groups.size())));
            Group& g = it->second;
            switch (r.below(4)) {
            case 0: g.a = std::move(g.b); break;                 // the refresh installed
            case 1: g.b = randomRoute(r, W, H); break;           // a refresh started
            case 2: g.a.reset(); g.b.reset(); break;             // evicted / widened
            default: g.a = randomRoute(r, W, H); g.b.reset();    // a shared field installed
            }
            relist(it->first, g);
        } else if (op < 8) {   // the group is gone
            auto it = groups.begin();
            std::advance(it, r.below(int(groups.size())));
            index.unlist(it->first);
            CHECK(!index.listed(it->first));
            groups.erase(it);
        } else {   // a static change: one sync's boxes on one plane
            const int plane = r.below(planes);
            std::vector<std::array<int, 4>> boxes;
            for (int n = r.range(1, 4); n > 0; --n) boxes.push_back(randomBox(r, W, H));
            std::vector<int> out, seen;
            const uint64_t stale = index.staleEntries(), queries = index.stats().queries;
            for (const auto& b : boxes) index.collect(plane, b, seen);   // the verification query: read-only
            CHECK(index.staleEntries() == stale && index.stats().queries == queries);
            std::sort(seen.begin(), seen.end());
            seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
            index.begin();
            size_t estimate = 0;
            for (const auto& b : boxes) estimate += index.estimate(plane, b);
            for (const auto& b : boxes) index.gather(plane, b, out);
            {
                std::vector<int> sorted = out;
                std::sort(sorted.begin(), sorted.end());
                CHECK(sorted == seen);   // collect() finds what gather() does
            }
            CHECK(out.size() <= estimate);
            std::set<int> got(out.begin(), out.end());
            CHECK(got.size() == out.size());   // each group once per query
            for (int id : got) {
                const auto at = groups.find(id);
                CHECK(at != groups.end() && at->second.plane == plane && (at->second.a || at->second.b));
            }
            for (const auto& [id, g] : groups) {
                if (g.plane != plane) continue;
                bool reached = false;
                for (const RouteGrid* f : {g.a.get(), g.b.get()})
                    for (const auto& b : boxes) reached |= f && routebins::touches(*f, b);
                if (reached && !got.count(id)) {
                    std::fprintf(stderr, "seed %llu step %d: group %d reached but not gathered\n", (unsigned long long)seed, step, id);
                    CHECK(false);
                }
            }
        }
        if (step % 97 == 0 || step == steps - 1) {
            CHECK(index.countsAgree());
            std::map<std::pair<int, int>, std::vector<int>> want;
            for (const auto& [id, g] : groups)
                if (g.plane >= 0 && (g.a || g.b))
                    for (int b : index.binsOf(g.a.get(), g.b.get())) want[{g.plane, b}].push_back(id);
            for (int p = 0; p < planes; ++p)
                for (int b = 0; b < index.binsAcross() * index.binsDown(); ++b) {
                    const auto at = want.find({p, b});
                    CHECK(index.liveIds(p, b) == (at == want.end() ? std::vector<int>{} : at->second));
                }
        }
    }
}
}  // namespace

int main() {
    {   // touches(): the window ring, the tile list, the empty box.
        RouteGrid f;
        f.initTiles(100, 10, 20, 30, 40, 4);   // cells x 10..39, z 20..59; 16-cell tiles 0..2 x 1..3
        CHECK(routebins::touches(f, {9, 19, 9, 19}));     // the diagonal neighbour of the corner
        CHECK(!routebins::touches(f, {8, 19, 8, 19}));
        CHECK(routebins::touches(f, {40, 60, 45, 70}));   // the far corner's neighbour
        CHECK(!routebins::touches(f, {41, 20, 41, 20}));
        CHECK(!routebins::touches(f, {0, 0, -1, -1}));    // no box
        f.allow.assign(size_t(f.sw) * size_t(f.sh), 0);
        f.allow[0] = 1;   // only tile (0,1): cells x 10..15, z 20..31
        f.bind();
        CHECK(routebins::touches(f, {16, 25, 16, 25}));   // beside the tile
        CHECK(!routebins::touches(f, {17, 25, 30, 25}));  // inside the window, two cells off the tile
        CHECK(routebins::touches(f, {9, 19, 9, 19}));
        routebins::Index ix;
        ix.size(100, 100);
        ix.list(5, 0, &f, nullptr);
        std::vector<int> out;
        ix.begin();
        ix.gather(0, {16, 25, 16, 25}, out);
        CHECK(out == std::vector<int>{5});
        out.clear();
        ix.begin();
        ix.gather(0, {80, 80, 90, 90}, out);   // a bin the tile never reaches
        CHECK(out.empty());
        ix.gather(1, {16, 25, 16, 25}, out);   // another plane
        CHECK(out.empty());
        ix.unlist(5);
        ix.begin();
        ix.gather(0, {16, 25, 16, 25}, out);
        // The tile (grown by one: z 19..32) listed two bins; the query read one and dropped its stale entry.
        CHECK(out.empty() && ix.countsAgree() && ix.staleEntries() == 1);
    }
    // Map sizes: a multiple of the bin, odd sizes, smaller than one bin, a long strip.
    const int sizes[][2] = {{64, 64}, {100, 37}, {20, 20}, {1190, 694}, {33, 300}, {832, 832}};
    uint64_t seed = 1;
    for (const auto& s : sizes)
        for (int planes = 1; planes <= 3; planes += 2) run(seed++, s[0], s[1], planes, 3000);
    {   // Compaction: enough churn of large routes that stale entries pass the slack several times.
        routebins::Index ix;
        ix.size(1024, 1024);
        RouteGrid big;
        big.initTiles(1024, 0, 0, 1024, 1024, 5);   // every bin
        int last[8] = {};
        for (int i = 0; i < 400; ++i) {
            ix.list(1 + i % 7, i % 2, &big, nullptr);
            last[1 + i % 7] = i % 2;
        }
        CHECK(ix.stats().compactions > 0 && ix.countsAgree());
        CHECK(ix.liveEntries() == 7u * 32u * 32u);
        std::vector<int> out, want;
        for (int id = 1; id <= 7; ++id)
            if (last[id] == 0) want.push_back(id);
        ix.begin();
        ix.gather(0, {500, 500, 500, 500}, out);
        std::sort(out.begin(), out.end());
        CHECK(out == want);   // the ids last listed on plane 0
    }
    {   // A new map size drops every listing.
        routebins::Index ix;
        ix.size(64, 64);
        RouteGrid f;
        f.initTiles(64, 0, 0, 10, 10, 5);
        ix.list(3, 0, &f, nullptr);
        CHECK(ix.listed(3));
        CHECK(ix.size(128, 64));
        CHECK(!ix.listed(3) && ix.liveEntries() == 0 && ix.countsAgree());
    }
    if (failures) {
        std::fprintf(stderr, "legion_routebins_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("legion_routebins_test: ok\n");
    return 0;
}
