#pragma once

// Persistent allocation of shadow silhouette tiles (rendering optimization
// workstream F).
//
// Before F, GameView repacked every visible shadow into height-sorted shelves
// every frame and compared each tile with whatever tile had the same INDEX on
// its page last frame. One unit entering view, leaving it, or growing by a
// pixel shifted every later tile of that page, so tiles whose content was
// provably unchanged (workstream E) were redrawn because they had moved or had
// been handed to another unit.
//
// Here each unit's tile lives in a CELL it keeps from frame to frame:
//
//   * Pages are logical W x H rectangles (the caller owns the GPU textures,
//     indexed by page). Page slots are stable; a released page leaves a hole
//     that a later page reuses.
//   * A page is cut into horizontal SHELVES (free vertical space is a sorted
//     list of row spans); a shelf holds cells side by side (its free space is a
//     sorted list of x spans). Freeing a cell merges its span back; an empty
//     shelf returns its rows to the page.
//   * Shelf heights come in classes (shadowCellHeight: steps of 2 px up to 32,
//     4 px up to 64, then 8 per octave), so tiles of similar height share a
//     shelf and a freed cell fits the next tile of that class. Widths are exact.
//     A unit whose tile outgrows its cell moves alone; nobody else moves. (Its
//     tile is redrawn anyway: bounds change only with coverage or placement.
//     Slack per cell was measured -- workstream F in the progress document --
//     and only cost pages: it saved no redraw.)
//   * Handles are generational (index + generation from ONE counter that is
//     never reset, not even by reset()), so a stale handle never aliases a new
//     cell, across compaction, page loss and renderer resets.
//   * Sequential mode is the former per-frame packer verbatim (a cursor per
//     page: append to the last shelf, else open the next row, page by page,
//     with the shelf growing to the tallest tile). After releaseAll(), feeding
//     the tiles sorted by height with exact sizes reproduces the pre-F slot of
//     every tile; compaction uses it so it never fits fewer tiles than the
//     former packer did.
//
// The allocator only places rectangles. Coverage validity (is the pixel
// content of a cell still what this frame would rasterize?) is decided by the
// tile stamp (client/shadowtiles.h) the caller keeps per handle index.
//
// Everything here is presentation state on the render thread; nothing reaches
// the simulation.

#include "client/rendertelemetry.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tak::render {

// Round a tile height (pixels, padding included) up to its shelf class:
// multiples of 2 up to 32, of 4 up to 64, then 8 steps per octave (<= 12.5%).
inline int shadowCellClass(int n) {
    if (n <= 2) return 2;
    const int b = std::bit_width(unsigned(n - 1));          // 2^(b-1) < n <= 2^b
    const int step = std::max(2, 1 << std::max(0, b - 4));
    return (n + step - 1) / step * step;
}
inline int shadowCellHeight(int n, int limit) { return std::min(limit, shadowCellClass(n)); }
// A unit keeps its cell while its tile fits and the cell is at most 1.5x the
// area a fresh cell would take (a shrunken tile returns the space).
inline bool shadowCellKeeps(int cellW, int cellH, int tileW, int tileH, int limitH) {
    return tileW <= cellW && tileH <= cellH &&
           2 * uint64_t(cellW) * uint64_t(cellH) <= 3 * uint64_t(tileW) * uint64_t(shadowCellHeight(tileH, limitH));
}

// ---- Page budget (perf follow-up K) ----
//
// Until K the atlas stopped at four pages: a view whose tiles needed more fell
// back to direct shadows for the WHOLE frame -- every triangle darkening the
// ground on its own (folds twice, the look the per-unit silhouettes avoid) on the
// most expensive shadow path -- and a view that filled them repacked (relocated
// and redrew) every tile each frame a tile outgrew its cell. Pages past the first
// kShadowBasePages are EXTENDED: added only where the base pages cannot hold the
// frame's tiles or are full (not merely fragmented) when a tile needs a cell, up to
// kShadowMaxPages, and only while the GPU budget (gpuvram) has room to spare:
//
//   * a base page needs only to fit the cap (gpuvram::wouldFit), as before;
//   * an extended page replaces a slower path -- drawing never depends on it --
//     so, like the body arena's storage, it may take the tracked total to at most
//     three quarters of the cap (the caches that obey the cap keep their room), and
//     none is added while the cap is tightened after a failed allocation;
//   * held extended pages yield under pressure (the cap tightened, or the total
//     above seven eighths of it): GameView resets the atlas and builds it on base
//     pages only (or falls back) for a cooldown. Admission at 3/4 and yield at 7/8
//     are the hysteresis: the atlas never grows into a total that makes it yield.
//
// Pages are counted live + pending (placed this frame, texture not yet created),
// and so are their bytes: a frame's pages are budgeted before any is created.
// kShadowMaxPages bounds them all (256 MiB of 2048^2 pages); equal to
// kShadowBasePages it means no extended page, the pre-K atlas (debug
// TAK_SHADOW_PAGES_MAX=4, for tests and same-binary comparisons).
inline constexpr size_t kShadowBasePages = 4;    // the former fixed limit
inline constexpr size_t kShadowMaxPages = 16;

struct ShadowGpuBudget {
    size_t held = 0;            // tracked GPU bytes now (gpuvram::bytes())
    size_t cap = SIZE_MAX;      // gpuvram::cap(); SIZE_MAX = unlimited
    bool tightened = false;     // gpuvram::tightened()
};
// May the page with ordinal `index` (live + pending pages before it) be added,
// `pending` bytes of earlier pages of this frame not created yet?
inline bool shadowPageFits(size_t index, size_t pending, size_t pageBytes, const ShadowGpuBudget& b,
                           size_t basePages = kShadowBasePages) {
    const auto fits = [&](size_t limit) {    // held + pending + pageBytes <= limit, overflow-safe
        return pending <= limit && pageBytes <= limit - pending && b.held <= limit - pending - pageBytes;
    };
    if (index < basePages) return fits(b.cap);
    return !b.tightened && fits(b.cap / 4 * 3);
}
// Must the extended pages the atlas holds be given back now?
inline bool shadowExtendedPressure(const ShadowGpuBudget& b) {
    return b.tightened || b.held > b.cap / 8 * 7;
}

struct ShadowCell {
    int page = -1, x = 0, y = 0, w = 0, h = 0;
    friend bool operator==(const ShadowCell&, const ShadowCell&) = default;
};

// One live allocation. ownerKey/entry/lastFrame/lastMs belong to the caller
// (identity and retention bookkeeping); the allocator only stores them.
struct ShadowAllocation {
    ShadowCell cell;
    uint64_t ownerKey = 0;
    const void* entry = nullptr;
    uint64_t lastFrame = 0;
    uint64_t lastMs = 0;
};

struct ShadowAtlasStats {
    size_t pages = 0, cells = 0, shelves = 0;
    uint64_t capacityPx = 0;     // live pages * W * H
    uint64_t cellPx = 0;         // allocated cell area
    uint64_t shelfPx = 0;        // area of open shelves
    uint64_t shelfFreePx = 0;    // free spans inside open shelves (fragmentation)
    uint64_t rowFreePx = 0;      // rows no shelf uses
    int largestFreeRows = 0;     // tallest free row span on any page
};

class ShadowAtlasAllocator {
public:
    // Drops every page and allocation (renderer reset, page size change).
    // Outstanding handles become invalid; generations keep counting.
    void reset(int pageW, int pageH) {
        pageW_ = pageW; pageH_ = pageH;
        pages_.clear();
        for (uint32_t i = 0; i < live_.size(); ++i) if (live_[i]) freeIndex(i);
    }
    int pageW() const { return pageW_; }
    int pageH() const { return pageH_; }
    size_t pageSlots() const { return pages_.size(); }
    bool pageLive(size_t p) const { return p < pages_.size() && pages_[p].live; }
    size_t livePages() const {
        size_t n = 0;
        for (const auto& p : pages_) n += p.live;
        return n;
    }
    size_t pageCells(size_t p) const { return pageLive(p) ? pages_[p].cells : 0; }
    uint64_t pageCellPx(size_t p) const { return pageLive(p) ? pages_[p].cellPx : 0; }

    // A new empty page; returns its slot (the lowest released slot first).
    int addPage() {
        size_t p = 0;
        while (p < pages_.size() && pages_[p].live) ++p;
        if (p == pages_.size()) pages_.emplace_back();
        Page& page = pages_[p];
        page = Page{};
        page.live = true;
        page.rows.push_back({0, pageH_});
        return int(p);
    }
    // Only an empty page can go; trailing holes are trimmed.
    bool releasePage(size_t p) {
        if (!pageLive(p) || pages_[p].cells) return false;
        pages_[p] = Page{};
        while (!pages_.empty() && !pages_.back().live) pages_.pop_back();
        return true;
    }

    // Frees every allocation and empties every page (compaction). Pages stay.
    // O(n): every index is free, and an ascending list is already a min-heap.
    void releaseAll() {
        std::fill(live_.begin(), live_.end(), false);
        free_.resize(live_.size());
        for (uint32_t i = 0; i < free_.size(); ++i) free_[i] = i;
        liveCount_ = 0;
        for (auto& page : pages_) if (page.live) {
            page.shelves.clear();
            page.rows.assign(1, {0, pageH_});
            page.cells = 0; page.cellPx = 0;
        }
    }

    // Places a w x h cell (padding included; the caller chooses classed or exact
    // heights) on a live page below pageLimit. Best fit (incremental), page by
    // page from the first: on that page the lowest shelf no more than an eighth
    // taller (>= 2 px) with a free span wide enough, else a new shelf in its
    // first rows that fit; only then the next page. Filling low pages first
    // keeps painter-order neighbours on one page (fewer composite batch breaks)
    // and lets the last page empty. Sequential: the former packer's cursor.
    // Returns an invalid handle when nothing fits.
    RenderHandle allocate(int w, int h, bool sequential, size_t pageLimit = SIZE_MAX) {
        if (w <= 0 || h <= 0 || w > pageW_ || h > pageH_) return {};
        const size_t end = std::min(pageLimit, pages_.size());
        return sequential ? allocateSequential(w, h, end) : allocateBestFit(w, h, end);
    }
    bool release(RenderHandle h) {
        if (!valid(h)) return false;
        ShadowAllocation& a = records_[h.index];
        Page& page = pages_[size_t(a.cell.page)];
        auto shelf = findShelf(page, a.cell.y);
        insertSpan(shelf->free, {a.cell.x, a.cell.x + a.cell.w});
        --shelf->cells;
        --page.cells; page.cellPx -= uint64_t(a.cell.w) * uint64_t(a.cell.h);
        if (!shelf->cells) {
            insertSpan(page.rows, {shelf->y, shelf->y + shelf->h});
            page.shelves.erase(shelf);
        }
        freeIndex(h.index);
        return true;
    }
    bool valid(RenderHandle h) const {
        return h.valid() && h.index < live_.size() && live_[h.index] && generations_[h.index] == h.generation;
    }
    ShadowAllocation* get(RenderHandle h) { return valid(h) ? &records_[h.index] : nullptr; }
    const ShadowAllocation* get(RenderHandle h) const { return valid(h) ? &records_[h.index] : nullptr; }
    // Upper bound of handle indices (size for caller arrays indexed by handle.index).
    uint32_t indexBound() const { return uint32_t(live_.size()); }
    size_t liveCells() const { return liveCount_; }
    template <class F> void forEach(F&& f) {
        for (uint32_t i = 0; i < live_.size(); ++i)
            if (live_[i]) f(RenderHandle{i, generations_[i]}, records_[i]);
    }
    // Drops free handle slots at the end of the table (memory follows demand).
    void trimHandles() {
        size_t n = live_.size();
        while (n && !live_[n - 1]) --n;
        if (n == live_.size()) return;
        records_.resize(n); generations_.resize(n); live_.resize(n);
        free_.erase(std::remove_if(free_.begin(), free_.end(), [&](uint32_t i) { return i >= n; }), free_.end());
        std::make_heap(free_.begin(), free_.end(), std::greater<>{});
    }
    size_t retainedBytes() const {
        size_t bytes = records_.capacity() * sizeof(ShadowAllocation) + generations_.capacity() * 4 +
                       live_.capacity() / 8 + free_.capacity() * 4 + pages_.capacity() * sizeof(Page);
        for (const auto& p : pages_) {
            bytes += p.rows.capacity() * sizeof(Span) + p.shelves.capacity() * sizeof(Shelf);
            for (const auto& s : p.shelves) bytes += s.free.capacity() * sizeof(Span);
        }
        return bytes;
    }

    ShadowAtlasStats stats() const {
        ShadowAtlasStats s;
        s.cells = liveCount_;
        for (const auto& page : pages_) if (page.live) {
            ++s.pages;
            s.capacityPx += uint64_t(pageW_) * uint64_t(pageH_);
            s.cellPx += page.cellPx;
            for (const auto& r : page.rows) {
                s.rowFreePx += uint64_t(r.x1 - r.x0) * uint64_t(pageW_);
                s.largestFreeRows = std::max(s.largestFreeRows, r.x1 - r.x0);
            }
            for (const auto& shelf : page.shelves) {
                ++s.shelves;
                s.shelfPx += uint64_t(pageW_) * uint64_t(shelf.h);
                for (const auto& f : shelf.free) s.shelfFreePx += uint64_t(f.x1 - f.x0) * uint64_t(shelf.h);
            }
        }
        return s;
    }

    // Full consistency check (tests and TAK_SHADOW_CACHE_VERIFY): every live cell
    // inside its page and shelf, no two cells overlapping, shelves and free rows
    // tiling each page exactly, free spans disjoint from cells, counts exact.
    bool validate(std::string* why = nullptr) const {
        const auto bad = [&](const char* m) { if (why) *why = m; return false; };
        std::vector<std::vector<uint8_t>> grid(pages_.size());
        std::vector<size_t> cells(pages_.size());
        std::vector<uint64_t> cellPx(pages_.size());
        size_t live = 0;
        for (uint32_t i = 0; i < live_.size(); ++i) {
            if (!live_[i]) continue;
            ++live;
            const ShadowCell& c = records_[i].cell;
            if (c.page < 0 || size_t(c.page) >= pages_.size() || !pages_[size_t(c.page)].live)
                return bad("cell on a page that is not live");
            if (c.x < 0 || c.y < 0 || c.w <= 0 || c.h <= 0 || c.x + c.w > pageW_ || c.y + c.h > pageH_)
                return bad("cell outside its page");
            const Page& page = pages_[size_t(c.page)];
            const auto shelf = std::find_if(page.shelves.begin(), page.shelves.end(),
                                            [&](const Shelf& s) { return s.y == c.y; });
            if (shelf == page.shelves.end() || c.h > shelf->h) return bad("cell outside its shelf");
            auto& g = grid[size_t(c.page)];
            if (g.empty()) g.assign(size_t(pageW_) * size_t(pageH_), 0);
            for (int y = c.y; y < c.y + c.h; ++y)
                for (int x = c.x; x < c.x + c.w; ++x) {
                    uint8_t& v = g[size_t(y) * size_t(pageW_) + size_t(x)];
                    if (v) return bad("two cells overlap");
                    v = 1;
                }
            ++cells[size_t(c.page)];
            cellPx[size_t(c.page)] += uint64_t(c.w) * uint64_t(c.h);
        }
        if (live != liveCount_) return bad("live count differs");
        for (size_t p = 0; p < pages_.size(); ++p) {
            const Page& page = pages_[p];
            if (!page.live) {
                if (cells[p] || !page.shelves.empty()) return bad("released page holds cells");
                continue;
            }
            if (cells[p] != page.cells || cellPx[p] != page.cellPx) return bad("page cell count differs");
            // Shelves and free rows must tile [0, H) exactly, in order.
            std::vector<Span> rows = page.rows;
            for (const auto& s : page.shelves) rows.push_back({s.y, s.y + s.h});
            std::sort(rows.begin(), rows.end(), [](const Span& a, const Span& b) { return a.x0 < b.x0; });
            int y = 0;
            for (const auto& r : rows) {
                if (r.x0 != y || r.x1 <= r.x0) return bad("shelves and free rows do not tile the page");
                y = r.x1;
            }
            if (y != pageH_) return bad("shelves and free rows do not cover the page");
            for (size_t i = 1; i < page.shelves.size(); ++i)
                if (page.shelves[i - 1].y >= page.shelves[i].y) return bad("shelves out of order");
            for (const auto& s : page.shelves) {
                if (!s.cells) return bad("empty shelf kept");
                int x = -1;
                size_t shelfCells = 0;
                for (const auto& f : s.free) {
                    if (f.x0 <= x || f.x1 <= f.x0 || f.x1 > pageW_) return bad("free spans overlap or unsorted");
                    x = f.x1;
                    // A free span holds no cell pixel (checked on the shelf's first row).
                    if (!grid[p].empty())
                        for (int fx = f.x0; fx < f.x1; ++fx)
                            if (grid[p][size_t(s.y) * size_t(pageW_) + size_t(fx)]) return bad("free span overlaps a cell");
                }
                for (uint32_t i = 0; i < live_.size(); ++i)
                    if (live_[i] && records_[i].cell.page == int(p) && records_[i].cell.y == s.y) ++shelfCells;
                if (shelfCells != s.cells) return bad("shelf cell count differs");
            }
        }
        return true;
    }

private:
    struct Span { int x0, x1; };   // [x0, x1): x on a shelf, y for free rows
    struct Shelf {
        int y = 0, h = 0;
        size_t cells = 0;
        std::vector<Span> free;    // sorted, disjoint
    };
    struct Page {
        bool live = false;
        std::vector<Shelf> shelves;   // sorted by y
        std::vector<Span> rows;       // free rows, sorted, disjoint
        size_t cells = 0;
        uint64_t cellPx = 0;
    };

    static void insertSpan(std::vector<Span>& spans, Span s) {
        auto it = std::lower_bound(spans.begin(), spans.end(), s.x0,
                                   [](const Span& a, int x) { return a.x0 < x; });
        it = spans.insert(it, s);
        if (it + 1 != spans.end() && it->x1 == (it + 1)->x0) { it->x1 = (it + 1)->x1; spans.erase(it + 1); }
        if (it != spans.begin() && (it - 1)->x1 == it->x0) { (it - 1)->x1 = it->x1; spans.erase(it); }
    }
    static std::vector<Shelf>::iterator findShelf(Page& page, int y) {
        return std::lower_bound(page.shelves.begin(), page.shelves.end(), y,
                                [](const Shelf& s, int v) { return s.y < v; });
    }

    RenderHandle place(size_t p, Shelf& shelf, size_t span, int w, int h) {
        Span& f = shelf.free[span];
        const int x = f.x0;
        f.x0 += w;
        if (f.x0 == f.x1) shelf.free.erase(shelf.free.begin() + ptrdiff_t(span));
        ++shelf.cells;
        Page& page = pages_[p];
        ++page.cells; page.cellPx += uint64_t(w) * uint64_t(h);
        const uint32_t i = takeIndex();
        records_[i] = ShadowAllocation{};
        records_[i].cell = {int(p), x, shelf.y, w, h};
        return {i, generations_[i]};
    }
    // A shelf of height h at the lowest free rows that fit; returns its index.
    static size_t openShelf(Page& page, size_t row, int h, int pageW) {
        Span& r = page.rows[row];
        Shelf shelf;
        shelf.y = r.x0; shelf.h = h;
        shelf.free.push_back({0, pageW});
        r.x0 += h;
        if (r.x0 == r.x1) page.rows.erase(page.rows.begin() + ptrdiff_t(row));
        const auto at = page.shelves.insert(findShelf(page, shelf.y), std::move(shelf));
        return size_t(at - page.shelves.begin());   // begin() after the insert may reallocate
    }

    RenderHandle allocateBestFit(int w, int h, size_t end) {
        const int tolerance = std::max(2, h / 8);
        for (size_t p = 0; p < end; ++p) {
            Page& page = pages_[p];
            if (!page.live) continue;
            size_t bestShelf = SIZE_MAX, bestSpan = 0;
            int bestH = INT_MAX;
            for (size_t s = 0; s < page.shelves.size() && bestH != h; ++s) {
                const Shelf& shelf = page.shelves[s];
                if (shelf.h < h || shelf.h > h + tolerance || shelf.h >= bestH) continue;
                for (size_t f = 0; f < shelf.free.size(); ++f)
                    if (shelf.free[f].x1 - shelf.free[f].x0 >= w) {
                        bestShelf = s; bestSpan = f; bestH = shelf.h;
                        break;
                    }
            }
            if (bestShelf != SIZE_MAX) {
                Shelf& shelf = page.shelves[bestShelf];
                const int cellH = shelf.h;
                return place(p, shelf, bestSpan, w, cellH);
            }
            for (size_t r = 0; r < page.rows.size(); ++r)
                if (page.rows[r].x1 - page.rows[r].x0 >= h) {
                    const size_t s = openShelf(page, r, h, pageW_);
                    return place(p, page.shelves[s], 0, w, h);
                }
        }
        return {};
    }

    // The former packer: per page, the last shelf is the open row. A tile that
    // fits its remaining width goes there (the row grows to the tallest tile, as
    // the former rowHeight=max(row, h) did); otherwise the next row starts below
    // it; a page whose rows are exhausted is skipped. Valid on pages filled only
    // by this mode since releaseAll() (the caller guarantees it).
    RenderHandle allocateSequential(int w, int h, size_t end) {
        for (size_t p = 0; p < end; ++p) {
            Page& page = pages_[p];
            if (!page.live) continue;
            if (page.shelves.empty()) {
                if (page.rows.empty() || page.rows.front().x0 != 0 || h > page.rows.front().x1) continue;
                const size_t s = openShelf(page, 0, h, pageW_);
                return place(p, page.shelves[s], 0, w, h);
            }
            Shelf& last = page.shelves.back();
            const int rowEnd = last.y + last.h;
            const bool rowsBelow = !page.rows.empty() && page.rows.back().x0 == rowEnd;
            const int below = rowsBelow ? page.rows.back().x1 - rowEnd : 0;
            const bool open = !last.free.empty() && last.free.back().x1 == pageW_;
            if (open && last.free.back().x0 + w <= pageW_) {
                if (h > last.h) {                 // the row grows to the tallest tile
                    if (last.y + h > pageH_ || h - last.h > below) continue;
                    Span& r = page.rows.back();
                    r.x0 += h - last.h;
                    if (r.x0 == r.x1) page.rows.pop_back();
                    last.h = h;
                }
                return place(p, last, last.free.size() - 1, w, h);
            }
            if (!rowsBelow || h > below) continue;  // the next row would not fit on this page
            const size_t s = openShelf(page, page.rows.size() - 1, h, pageW_);
            return place(p, page.shelves[s], 0, w, h);
        }
        return {};
    }

    uint32_t takeIndex() {
        uint32_t i;
        if (!free_.empty()) {
            std::pop_heap(free_.begin(), free_.end(), std::greater<>{});
            i = free_.back(); free_.pop_back();
        } else {
            i = uint32_t(live_.size());
            records_.emplace_back(); generations_.push_back(0); live_.push_back(false);
        }
        if (++nextGeneration_ == 0) ++nextGeneration_;   // 0 never names a live cell
        generations_[i] = nextGeneration_;
        live_[i] = true;
        ++liveCount_;
        return i;
    }
    void freeIndex(uint32_t i) {
        live_[i] = false;
        free_.push_back(i);
        std::push_heap(free_.begin(), free_.end(), std::greater<>{});
        --liveCount_;
    }

    int pageW_ = 0, pageH_ = 0;
    std::vector<Page> pages_;
    std::vector<ShadowAllocation> records_;
    std::vector<uint32_t> generations_;
    std::vector<bool> live_;
    std::vector<uint32_t> free_;     // min-heap: lowest free index first
    size_t liveCount_ = 0;
    uint32_t nextGeneration_ = 0;
};

}  // namespace tak::render
