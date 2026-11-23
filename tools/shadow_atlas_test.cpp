// Persistent shadow tile allocation (client/shadowatlas.h), workstream F.
//
// Properties checked:
//   * height classes round up, never shrink a request, waste <= 12.5% above 64 px;
//     the keep rule holds a tile while it fits and returns oversized cells;
//   * SEQUENTIAL mode after releaseAll() reproduces the former per-frame packer
//     (GameView::buildShadowSilhouetteAtlas at cfb11f36, copied below verbatim)
//     slot for slot, including where it opens pages and where it runs out;
//   * randomized allocate / release / resize / page churn never produces
//     overlapping cells, cells outside a page or shelf, or inconsistent free
//     lists (validate() after every step), and live cells never move;
//   * handles are generational: a released or reset handle is invalid, and no
//     generation is ever handed out twice (also across reset());
//   * freeing everything returns every page to one free row (full reclamation);
//   * statistics tile the capacity exactly.
#include "client/shadowatlas.h"

#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace tak::render;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ---- the former packer (cfb11f36), verbatim except for the container types ----
struct FormerPage { int x = 0, y = 0, rowHeight = 0; };
struct FormerTile { int width, height, page = -1, slotX = 0, slotY = 0; };
// Returns false where the former code returned "page limit reached".
bool formerPack(std::vector<FormerTile>& work, int pageW, int pageH, size_t kMaxPages, size_t& pagesUsed) {
    std::vector<FormerPage> pages;
    std::stable_sort(work.begin(), work.end(), [](const FormerTile& a, const FormerTile& b) {
        return a.height != b.height ? a.height > b.height : a.width > b.width;
    });
    for (FormerTile& w : work) {
        size_t chosen = SIZE_MAX;
        int slotX = 0, slotY = 0;
        auto tryPage = [&](size_t p) {
            auto& page = pages[p];
            int x = page.x, y = page.y, row = page.rowHeight;
            if (x + w.width + 2 > pageW) { x = 0; y += row; row = 0; }
            if (y + w.height + 2 > pageH) return false;
            slotX = x; slotY = y;
            page.x = x + w.width + 2; page.y = y;
            page.rowHeight = std::max(row, w.height + 2);
            chosen = p;
            return true;
        };
        for (size_t p = 0; p < pages.size(); ++p)
            if (tryPage(p)) break;
        if (chosen == SIZE_MAX) {
            if (pages.size() >= kMaxPages) return false;
            pages.push_back({});
            if (!tryPage(pages.size() - 1)) return false;
        }
        w.page = int(chosen); w.slotX = slotX; w.slotY = slotY;
    }
    pagesUsed = pages.size();
    return true;
}

// The same tiles through the allocator: releaseAll, sorted, sequential, exact
// sizes (+2 padding), a page added whenever nothing fits (as GameView does).
bool newPack(ShadowAtlasAllocator& a, std::vector<FormerTile>& work, size_t kMaxPages) {
    a.releaseAll();
    std::stable_sort(work.begin(), work.end(), [](const FormerTile& x, const FormerTile& y) {
        return x.height != y.height ? x.height > y.height : x.width > y.width;
    });
    for (FormerTile& w : work) {
        RenderHandle h = a.allocate(w.width + 2, w.height + 2, true);
        if (!h.valid()) {
            if (a.livePages() >= kMaxPages) return false;
            a.addPage();
            h = a.allocate(w.width + 2, w.height + 2, true);
            if (!h.valid()) return false;
        }
        const ShadowCell& c = a.get(h)->cell;
        w.page = c.page; w.slotX = c.x; w.slotY = c.y;
    }
    return true;
}

void testClasses() {
    bool ok = true, waste = true;
    int prev = 0;
    for (int n = 1; n <= 4096; ++n) {
        const int c = shadowCellClass(n);
        ok &= c >= n && c >= prev;
        if (n > 64) waste &= double(c - n) <= 0.125 * n + 1e-9;
        else waste &= c - n < (n > 32 ? 4 : 2);
        prev = c;
        ok &= shadowCellClass(c) == c;   // idempotent
    }
    check(ok, "height class >= request, monotone, idempotent");
    check(waste, "height class waste < 4 px up to 64, <= 12.5% above");
    check(shadowCellHeight(4000, 2048) == 2048, "class clamped to the page");
    check(shadowCellClass(17) == 18 && shadowCellClass(33) == 36 && shadowCellClass(65) == 72, "class steps 2 / 4 / 8");
    check(shadowCellKeeps(40, 40, 40, 40, 2048), "a tile keeps an exact cell");
    check(!shadowCellKeeps(40, 40, 41, 40, 2048) && !shadowCellKeeps(40, 40, 40, 41, 2048), "an outgrown cell is left");
    check(shadowCellKeeps(40, 40, 36, 38, 2048), "a slightly smaller tile keeps its cell");
    check(!shadowCellKeeps(40, 40, 20, 40, 2048), "a cell twice the tile is returned");
}

void testFormerEquivalence() {
    std::mt19937 rng(1234);
    size_t scenes = 0, failing = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const int dims[] = {256, 512, 1024, 2048};
        const int pageW = dims[rng() % 4], pageH = dims[rng() % 4];
        const size_t maxPages = 1 + rng() % 4;
        const int n = 1 + int(rng() % 600);
        const int maxSide = 4 + int(rng() % 120);
        std::vector<FormerTile> tiles;
        for (int i = 0; i < n; ++i) {
            const int w = 1 + int(rng() % unsigned(std::min(maxSide, pageW - 2)));
            const int h = 1 + int(rng() % unsigned(std::min(maxSide, pageH - 2)));
            tiles.push_back({w, h});
        }
        std::vector<FormerTile> former = tiles, now = tiles;
        size_t pagesUsed = 0;
        const bool fit = formerPack(former, pageW, pageH, maxPages, pagesUsed);
        ShadowAtlasAllocator a;
        a.reset(pageW, pageH);
        // A page kept from an earlier frame is tried first, exactly like the page
        // the former packer would create for the first tile.
        if (rng() % 2) a.addPage();
        const bool fitNow = newPack(a, now, maxPages);
        ++scenes;
        failing += !fit;
        if (fit != fitNow) {
            check(false, "sequential mode fits exactly where the former packer fits");
            continue;
        }
        if (!fit) continue;
        bool same = true;
        for (size_t i = 0; i < former.size(); ++i)
            same &= former[i].page == now[i].page && former[i].slotX == now[i].slotX && former[i].slotY == now[i].slotY;
        check(same, "sequential mode reproduces every former slot");
        std::string why;
        { const bool ok = a.validate(&why); check(ok, ("validate: " + why).c_str()); }
    }
    std::printf("former packer equivalence: %zu scenes (%zu over the page limit)\n", scenes, failing);
}

void testRandomChurn() {
    std::mt19937 rng(77);
    ShadowAtlasAllocator a;
    a.reset(512, 512);
    struct Live { RenderHandle h; ShadowCell cell; };
    std::vector<Live> live;
    std::set<uint32_t> generations;
    std::vector<RenderHandle> dead;
    bool unique = true, stable = true, staleInvalid = true;
    std::string why;
    bool valid = true;
    size_t allocations = 0, failed = 0;
    for (int step = 0; step < 20000; ++step) {
        const unsigned op = rng() % 10;
        if (op < 5 || live.empty()) {
            const int w = 1 + int(rng() % 60), h = shadowCellHeight(1 + int(rng() % 60), 512);
            RenderHandle hd = a.allocate(w, h, false);
            if (!hd.valid() && a.livePages() < 4) { a.addPage(); hd = a.allocate(w, h, false); }
            if (!hd.valid()) { ++failed; continue; }
            ++allocations;
            unique &= generations.insert(hd.generation).second;
            live.push_back({hd, a.get(hd)->cell});
        } else if (op < 9) {
            const size_t i = rng() % live.size();
            check(a.release(live[i].h), "release of a live handle succeeds");
            dead.push_back(live[i].h);
            live[i] = live.back(); live.pop_back();
        } else {
            // Release an empty page if there is one (page reclamation).
            for (size_t p = 0; p < a.pageSlots(); ++p)
                if (a.pageLive(p) && !a.pageCells(p)) { a.releasePage(p); break; }
        }
        for (const Live& l : live) {
            const ShadowAllocation* r = a.get(l.h);
            stable &= r && r->cell == l.cell;
        }
        for (size_t i = dead.size() > 64 ? dead.size() - 64 : 0; i < dead.size(); ++i) staleInvalid &= !a.valid(dead[i]);
        if (step % 50 == 0 && valid) valid = a.validate(&why);
        const auto s = a.stats();
        if (s.shelfPx + s.rowFreePx != s.capacityPx || s.cellPx > s.shelfPx - s.shelfFreePx) valid = false, why = "stats do not tile the capacity";
    }
    check(valid, ("random churn keeps the allocator consistent: " + why).c_str());
    check(unique, "no generation handed out twice");
    check(stable, "live cells never move");
    check(staleInvalid, "released handles are invalid");
    for (const Live& l : live) a.release(l.h);
    bool reclaimed = a.liveCells() == 0;
    for (size_t p = 0; p < a.pageSlots(); ++p) reclaimed &= !a.pageLive(p) || a.pageCells(p) == 0;
    const auto s = a.stats();
    reclaimed &= s.shelves == 0 && s.rowFreePx == s.capacityPx && s.largestFreeRows == 512;
    check(reclaimed, "freeing every cell returns every page to one free row");
    for (size_t p = a.pageSlots(); p-- > 0;) a.releasePage(p);
    check(a.livePages() == 0 && a.pageSlots() == 0, "empty pages release completely");
    a.trimHandles();
    check(a.indexBound() == 0, "handle table trims when nothing is live");
    std::printf("random churn: %zu allocations, %zu refused at the page limit\n", allocations, failed);
}

void testHandlesAndReset() {
    ShadowAtlasAllocator a;
    a.reset(256, 256);
    a.addPage();
    const RenderHandle h1 = a.allocate(20, 20, false);
    check(h1.valid() && a.valid(h1), "allocation valid");
    a.reset(256, 256);
    check(!a.valid(h1), "reset invalidates every handle");
    a.addPage();
    const RenderHandle h2 = a.allocate(20, 20, false);
    check(h2.index == h1.index && h2.generation != h1.generation, "index reused with a new generation after reset");
    check(!a.valid(h1) && a.valid(h2), "stale handle never aliases the new cell");
    a.releaseAll();
    check(!a.valid(h2) && a.livePages() == 1 && a.pageCells(0) == 0, "releaseAll keeps pages, frees cells");
    check(!a.releasePage(5), "releasing a missing page fails");
    const RenderHandle h3 = a.allocate(20, 20, false);
    check(!a.releasePage(0), "a page holding a cell cannot be released");
    a.release(h3);
    check(a.releasePage(0), "an empty page can be released");
    check(!a.allocate(20, 20, false).valid(), "no page: nothing fits");
    check(!a.allocate(300, 10, false).valid(), "wider than a page: refused");
}

void testStableNeighbours() {
    // A row of tiles; one of them grows by a pixel (within its slack), another
    // leaves and a new one arrives: no other cell moves (the former packer moved
    // every later tile of the page).
    ShadowAtlasAllocator a;
    a.reset(256, 256);
    a.addPage();
    std::vector<RenderHandle> hs;
    std::vector<ShadowCell> cells;
    for (int i = 0; i < 30; ++i) {
        hs.push_back(a.allocate(20 + i % 5, shadowCellHeight(24, 256), false));
        cells.push_back(a.get(hs.back())->cell);
    }
    // Tile 3 grows by one pixel: it alone moves to a new cell.
    a.release(hs[3]);
    hs[3] = a.allocate(24, shadowCellHeight(25, 256), false);
    a.release(hs[7]);
    const RenderHandle fresh = a.allocate(22, shadowCellHeight(24, 256), false);
    bool unmoved = true;
    for (int i = 0; i < 30; ++i) if (i != 7 && i != 3) unmoved &= a.get(hs[i])->cell == cells[i];
    check(unmoved, "a growing tile and a replaced tile leave every other cell in place");
    const ShadowCell f = a.get(fresh)->cell;
    check((f.x == cells[7].x || f.x == cells[3].x) && f.y == cells[7].y, "a freed cell is reused first");
    std::string why;
    { const bool ok = a.validate(&why); check(ok, ("validate: " + why).c_str()); }
}

void testPageLimitBestFit() {
    // Best fit uses an existing shelf of a slightly taller class before opening
    // a new shelf, and opens shelves only where rows are free.
    ShadowAtlasAllocator a;
    a.reset(64, 64);
    a.addPage();
    const RenderHandle t = a.allocate(16, 20, false);
    const RenderHandle s = a.allocate(16, 18, false);   // 20 <= 18 + 2: same shelf
    check(a.get(t)->cell.y == a.get(s)->cell.y, "shorter tile shares a shelf within the tolerance");
    check(a.get(s)->cell.h == 20, "cell takes the shelf height");
    const RenderHandle u = a.allocate(16, 8, false);    // 20 > 8 + 2: own shelf
    check(a.get(u)->cell.y != a.get(t)->cell.y, "much shorter tile opens its own shelf");
    std::vector<RenderHandle> fill;
    for (;;) { const RenderHandle h = a.allocate(16, 8, false); if (!h.valid()) break; fill.push_back(h); }
    check(!a.allocate(16, 8, false).valid(), "full page refuses");
    a.release(fill.back());
    check(a.allocate(16, 8, false).valid(), "freed cell is found again");
    std::string why;
    { const bool ok = a.validate(&why); check(ok, ("validate: " + why).c_str()); }
}

void testPageBudget() {
    // Perf follow-up K: base pages need only fit the cap (the former check,
    // gpuvram::wouldFit((pending + 1) * page)); extended pages must leave a quarter
    // of the cap and none is added while the cap is tightened; extended pages yield
    // when the cap is tightened or the total passes seven eighths of it.
    constexpr size_t MiB = size_t(1) << 20, page = 16 * MiB;
    const ShadowGpuBudget roomy{300 * MiB, 1280 * MiB, false};
    check(shadowPageFits(0, 0, page, roomy) && shadowPageFits(3, 3 * page, page, roomy), "base pages fit a roomy cap");
    check(shadowPageFits(4, 4 * page, page, roomy), "an extended page fits within 3/4 of the cap");
    // 300 + 26 * 16 = 716 <= 960 (3/4 of 1280); 300 + 42 * 16 = 972 > 960.
    check(shadowPageFits(25, 25 * page, page, roomy) && !shadowPageFits(41, 41 * page, page, roomy),
          "extended pages stop at three quarters of the cap, pending bytes counted");
    const ShadowGpuBudget tight{300 * MiB, 1280 * MiB, true};
    check(shadowPageFits(3, 0, page, tight) && !shadowPageFits(4, 0, page, tight),
          "a tightened cap admits base pages (as before) but no extended page");
    // The base check is exactly the former one: held + (pending + 1) * page <= cap.
    bool former = true;
    std::mt19937 rng(9);
    for (int i = 0; i < 20000; ++i) {
        const size_t cap = (64 + rng() % 4096) * MiB, held = (rng() % 4096) * MiB;
        const size_t pending = rng() % 8;
        const ShadowGpuBudget b{held, cap, bool(rng() % 2)};
        const size_t add = (pending + 1) * page;
        const bool wouldFit = add <= cap && held <= cap - add;
        former &= shadowPageFits(rng() % 4, pending * page, page, b) == wouldFit;
    }
    check(former, "base pages: the former budget check, bit for bit");
    // Hysteresis: whatever an extended admission allows never puts the total over
    // the yield line (the atlas does not grow into a pressure shed of its own).
    bool hysteresis = true, unlimited = true;
    for (int i = 0; i < 20000; ++i) {
        const size_t cap = (64 + rng() % 4096) * MiB, held = (rng() % 4096) * MiB;
        const size_t pending = (rng() % 16) * page;
        const ShadowGpuBudget b{held, cap, false};
        if (shadowPageFits(4 + rng() % 12, pending, page, b))
            hysteresis &= !shadowExtendedPressure({held + pending + page, cap, false});
        unlimited &= shadowPageFits(4 + rng() % 12, pending, page, {held, SIZE_MAX, false});
    }
    check(hysteresis, "an admitted extended page never reaches the yield line");
    check(unlimited, "an unlimited cap admits extended pages");
    check(!shadowPageFits(4, SIZE_MAX - page / 2, page, roomy) && !shadowPageFits(0, 0, SIZE_MAX, roomy),
          "huge requests are refused, not wrapped");
    check(shadowExtendedPressure(tight), "a tightened cap is pressure");
    check(!shadowExtendedPressure({1120 * MiB, 1280 * MiB, false}) && shadowExtendedPressure({1121 * MiB, 1280 * MiB, false}),
          "pressure above seven eighths of the cap");
    check(!shadowExtendedPressure({4096 * MiB, SIZE_MAX, false}), "no pressure without a cap");
    check(shadowPageFits(1, 0, page, roomy, 1) == shadowPageFits(4, 0, page, roomy) && !shadowPageFits(1, 0, page, tight, 1),
          "the base page count is a parameter (TAK_SHADOW_PAGES_MAX below four)");
}

}  // namespace

int main() {
    testClasses();
    testFormerEquivalence();
    testRandomChurn();
    testHandlesAndReset();
    testStableNeighbours();
    testPageLimitBestFit();
    testPageBudget();
    if (failures) { std::fprintf(stderr, "shadow_atlas: %d failure(s)\n", failures); return 1; }
    std::printf("shadow_atlas: all checks passed\n");
    return 0;
}
