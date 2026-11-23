// legion_cluster_test -- refreshPlane's dirty-rectangle clusters (src/sim/legion_cluster.h) against the reference:
// the pairwise union-find and per-root collection refreshPlane used before the bins (copied below verbatim).
//
// Checked, for both the automatic path and the binned path forced at every count: the same clusters, in the same
// order (by first rectangle), with the same rectangle order inside, on random lists -- sparse map-wide changes,
// dense pile-ups (many identical or nested rectangles), chains that join far ends only transitively, zero-size and
// negative-size rectangles, negative coordinates, boxes too large for the bins, grow from 1 to 40, and lists of
// up to 4096 (a background build's pending limit) and 1024+ (the per-tick dirty list's limit).
//
// `legion_cluster_test --bench` times reference vs clusterRects on fixed workloads, and `--bench-scn FILE...` on the
// rectangles of a .scn's churn lines (tools/scenarios/perf/clusterchurn-*.scn); screening numbers only.
#include "sim/legion_cluster.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace tak::sim;
using Rect = cluster::Rect;

namespace {
int failures = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            std::fprintf(stderr, "legion_cluster_test:%d: FAILED %s\n", __LINE__, #c); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

uint64_t rng = 0x9e3779b97f4a7c15ull;
uint32_t next() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return uint32_t(rng >> 11);
}
int range(int lo, int hi) {return lo + int(next() % uint32_t(hi - lo + 1));}

// refreshPlane at 461e9c4b, the clustering part: clusters as lists of rectangle indices, in visit order.
std::vector<std::vector<int>> reference(const std::vector<Rect>& rects, int grow) {
    std::vector<int> cluster(rects.size());
    for(size_t i=0;i<rects.size();++i)cluster[i]=int(i);
    auto root=[&](int i) {while(cluster[size_t(i)]!=i)i=cluster[size_t(i)]=cluster[size_t(cluster[size_t(i)])];return i;};
    for(size_t i=0;i<rects.size();++i)for(size_t j=i+1;j<rects.size();++j) {
        const auto& a=rects[i];const auto& b=rects[j];
        if(a[0]-grow<b[0]+b[2]+grow&&b[0]-grow<a[0]+a[2]+grow&&a[1]-grow<b[1]+b[3]+grow&&b[1]-grow<a[1]+a[3]+grow) {
            const int ra=root(int(i)),rb=root(int(j));
            if(ra!=rb)cluster[size_t(std::max(ra,rb))]=std::min(ra,rb);
        }
    }
    std::vector<std::vector<int>> out;
    for(size_t i=0;i<rects.size();++i) {
        if(root(int(i))!=int(i))continue;
        out.emplace_back();
        for(size_t j=i;j<rects.size();++j)if(root(int(j))==int(i))out.back().push_back(int(j));
    }
    return out;
}

std::vector<std::vector<int>> lists(const cluster::Clusters& c) {
    std::vector<std::vector<int>> out;
    for (size_t k = 0; k < c.clusters(); ++k)
        out.emplace_back(c.order.begin() + c.start[k], c.order.begin() + c.start[k + 1]);
    return out;
}

cluster::Clusters scratch;   // reused across calls, as refreshPlane reuses its scratch

void compare(const std::vector<Rect>& rects, int grow, const char* what) {
    const auto want = reference(rects, grow);
    cluster::clusterRects(rects, grow, scratch);
    const bool autoOk = lists(scratch) == want;
    cluster::clusterRects(rects, grow, scratch, true);
    const bool binOk = lists(scratch) == want;
    CHECK(autoOk);
    CHECK(binOk);
    if (!autoOk || !binOk)
        std::fprintf(stderr, "  case %s: n=%zu grow=%d reference clusters=%zu\n", what, rects.size(), grow, want.size());
}

// Workloads. `area` is the side of the square the rectangles fall in; sizes 1..maxSize.
std::vector<Rect> scatter(int n, int x0, int z0, int area, int maxSize) {
    std::vector<Rect> r;
    for (int i = 0; i < n; ++i)
        r.push_back({x0 + range(0, area), z0 + range(0, area), range(1, maxSize), range(1, maxSize)});
    return r;
}

void randomCases() {
    const int grows[] = {1, 2, 5, 9, 10, 13, 17, 24, 40};
    for (int iter = 0; iter < 3000; ++iter) {
        const int grow = grows[next() % 9];
        const int n = iter < 2000 ? range(0, 80) : range(0, 400);
        std::vector<Rect> rects;
        switch (iter % 7) {
        case 0: rects = scatter(n, 0, 0, 512, 6); break;                          // map-wide
        case 1: rects = scatter(n, 100, 100, 40, 4); break;                       // a battle's corpses
        case 2: rects = scatter(n, -30, -30, 3000, 9); break;                     // very sparse, negative origin
        case 3: {                                                                 // identical pile-ups
            const Rect a{range(0, 200), range(0, 200), range(0, 5), range(0, 5)};
            for (int i = 0; i < n; ++i) rects.push_back(next() % 4 ? a : Rect{a[0] + range(-30, 30), a[1] + range(-30, 30), 2, 2});
            break;
        }
        case 4: {                                                                 // chains: ends meet only transitively
            int x = range(0, 50), z = range(0, 50);
            for (int i = 0; i < n; ++i) {
                rects.push_back({x, z, range(1, 3), range(1, 3)});
                x += range(-1, 2 * grow + 4); z += range(-3, 3);
            }
            break;
        }
        case 5: {                                                                 // odd sizes: zero, negative, huge
            for (int i = 0; i < n; ++i) {
                const int k = range(0, 9);
                const int w = k == 0 ? 0 : k == 1 ? -range(1, 2 * grow + 6) : k == 2 ? range(100, 900) : range(1, 5);
                const int h = k == 3 ? 0 : k == 4 ? -range(1, 2 * grow + 6) : k == 5 ? range(100, 900) : range(1, 5);
                rects.push_back({range(-50, 400), range(-50, 400), w, h});
            }
            break;
        }
        default: {                                                                // exact boundary contacts
            for (int i = 0; i < n; ++i) {
                const int s = 2 * grow;
                rects.push_back({range(0, 20) * (s + 1) + range(-1, 1), range(0, 20) * (s + 1) + range(-1, 1), 1, 1});
            }
            break;
        }
        }
        compare(rects, grow, "random");
    }
}

void edgeCases() {
    compare({}, 10, "empty");
    compare({{5, 5, 1, 1}}, 10, "single");
    // Two boxes grown by 10 meet when 10 + 1 + 10 > gap: x 0 and 21 meet, 0 and 22 do not.
    compare({{0, 0, 1, 1}, {21, 0, 1, 1}}, 10, "touch");
    compare({{0, 0, 1, 1}, {22, 0, 1, 1}}, 10, "apart");
    // A chain through a middle rectangle that comes last: the first cluster takes it.
    compare({{0, 0, 1, 1}, {40, 0, 1, 1}, {500, 500, 1, 1}, {20, 0, 1, 1}}, 10, "late bridge");
    // Large lists at the engine's limits: 1024 dirty rectangles (+ structure stamps) and 4096 pending.
    for (const int n : {1024, 1100, 4096}) {
        compare(scatter(n, 0, 0, 512, 6), 13, "limit sparse");
        compare(scatter(n, 0, 0, 96, 3), 13, "limit dense");
    }
    // A box spanning more bins than the per-rectangle limit sits outside the bins (joined pairwise).
    std::vector<Rect> big = scatter(60, 0, 0, 2000, 3);
    big.push_back({300, 300, 1500, 1500});
    big.push_back({-4000, -4000, 2, 2});
    compare(big, 10, "huge box");
    // A coordinate span past the grid limit: everything pairwise.
    compare({{-1000000000, 0, 1, 1}, {1000000000, 0, 1, 1}, {0, 0, 1, 1}, {1000000000, 5, 2, 2}}, 10, "far span");
    std::vector<Rect> far = scatter(40, 0, 0, 100, 3);
    far.push_back({2000000000, 2000000000, 1, 1});
    compare(far, 10, "far span many");
}

// ---- bench (screening) ----
double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void bench() {
    struct W {const char* name; int n, x0, area, maxSize;};
    const W ws[] = {{"n2-sparse", 2, 0, 512, 4},      {"n4-sparse", 4, 0, 512, 4},      {"n8-sparse", 8, 0, 512, 4},
                    {"n16-sparse", 16, 0, 512, 4},    {"n32-sparse", 32, 0, 512, 4},    {"n48-sparse", 48, 0, 512, 4},
                    {"n64-sparse", 64, 0, 512, 4},    {"n96-sparse", 96, 0, 512, 4},    {"n16-dense", 16, 100, 48, 3},
                    {"n32-dense", 32, 100, 48, 3},    {"n48-dense", 48, 100, 56, 3},    {"n64-dense", 64, 100, 64, 3},
                    {"n96-dense", 96, 100, 72, 3},    {"n256-sparse", 256, 0, 512, 4},  {"n256-dense", 256, 100, 96, 3},
                    {"n1024-sparse", 1024, 0, 512, 4}, {"n1024-dense", 1024, 100, 128, 3},
                    {"n4096-sparse", 4096, 0, 512, 4}, {"n4096-dense", 4096, 100, 160, 3}};
    const int grow = 13;   // kRelabelMargin 8 + a 4-cell footprint + 1
    std::printf("%-14s %8s %12s %12s %8s %12s\n", "workload", "clusters", "ref us/call", "new us/call", "ratio", "bins us/call");
    for (const W& w : ws) {
        std::vector<std::vector<Rect>> sets;
        for (int k = 0; k < 16; ++k) sets.push_back(scatter(w.n, w.x0, w.x0, w.area, w.maxSize));
        const int reps = std::max(4, 200000 / std::max(1, w.n * w.n / 8 + w.n));
        size_t sink = 0;
        double ref = 1e30, cur = 1e30, bins = 1e30;
        for (int round = 0; round < 5; ++round) {
            double t0 = nowMs();
            for (int r = 0; r < reps; ++r) sink += reference(sets[size_t(r) % 16], grow).size();
            double t1 = nowMs();
            for (int r = 0; r < reps; ++r) {cluster::clusterRects(sets[size_t(r) % 16], grow, scratch); sink += scratch.clusters();}
            double t2 = nowMs();
            for (int r = 0; r < reps; ++r) {cluster::clusterRects(sets[size_t(r) % 16], grow, scratch, true); sink += scratch.clusters();}
            double t3 = nowMs();
            ref = std::min(ref, (t1 - t0) * 1000 / reps);
            cur = std::min(cur, (t2 - t1) * 1000 / reps);
            bins = std::min(bins, (t3 - t2) * 1000 / reps);
        }
        std::printf("%-14s %8zu %12.3f %12.3f %8.2f %12.3f\n", w.name, reference(sets[0], grow).size(), ref, cur, ref / cur, bins);
        if (sink == 42) std::printf("\n");
    }
}
// --bench-scn FILE: the rectangles a .scn's `churn` lines hand refreshPlane in one tick (each line's W x H box twice:
// the toggle's place and the lift that replaces it, as LCLUSTER counts them), per plane footprint 1..3 (grow 10..12).
void benchScn(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {std::fprintf(stderr, "cannot read %s\n", path); return;}
    std::vector<Rect> rects;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        int x, z, w, h;
        if (std::sscanf(line, "churn %d %d %d %d", &x, &z, &w, &h) == 4) rects.push_back({x, z, w, h});
    }
    std::fclose(f);
    const size_t lines = rects.size();
    for (size_t i = 0; i < lines; ++i) rects.push_back(rects[i]);
    std::printf("%s: %zu rectangles\n", path, rects.size());
    for (int foot = 1; foot <= 3; ++foot) {
        const int grow = 8 + foot + 1;
        const int reps = std::max(4, int(400000 / (rects.size() * rects.size() / 8 + rects.size())));
        size_t sink = 0;
        double ref = 1e30, cur = 1e30;
        for (int round = 0; round < 5; ++round) {
            const double t0 = nowMs();
            for (int r = 0; r < reps; ++r) sink += reference(rects, grow).size();
            const double t1 = nowMs();
            for (int r = 0; r < reps; ++r) {cluster::clusterRects(rects, grow, scratch); sink += scratch.clusters();}
            const double t2 = nowMs();
            ref = std::min(ref, (t1 - t0) * 1000 / reps);
            cur = std::min(cur, (t2 - t1) * 1000 / reps);
        }
        std::printf("  grow %d: clusters %zu  ref %.3f us  new %.3f us  ratio %.2f\n", grow, scratch.clusters(), ref, cur, ref / cur);
        if (sink == 42) std::printf("\n");
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {bench(); return 0;}
    if (argc > 2 && std::strcmp(argv[1], "--bench-scn") == 0) {for (int i = 2; i < argc; ++i) benchScn(argv[i]); return 0;}
    edgeCases();
    randomCases();
    if (failures) {std::fprintf(stderr, "legion_cluster_test: %d failures\n", failures); return 1;}
    std::printf("legion_cluster_test: ok\n");
    return 0;
}
