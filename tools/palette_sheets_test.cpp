// Workstream M: structure palette sheets (client/palettesheets.h), no SDL/GL.
//
//  1. PageLayout places every frame exactly where the pre-M packer did (a copy
//     of that loop below), including frames that do not fit and the page cap.
//  2. Every sheet origin is x % 4 == 0, y % 5 == 0, so the indexed rectangles
//     (x/4, y/5, w, h) are exact, disjoint and inside the index rows.
//  3. The sheet id survives in texture coordinates exactly: for every baked
//     coordinate k/2048 and every sheet id, decoding returns both bit-exactly.
//  4. Sheet-table entries round-trip.
//  5. resolveTexel (the shader's decode) equals the baked expansion at every
//     texel of every sheet of a random roster; outside a sheet it clamps.
//  6. IndexPageModel::resolve (the whole decode, owner lookup included) equals
//     the baked page at EVERY texel of random full pages, whichever sheet the
//     sampling polygon belongs to: inside its own sheet, in a neighbouring
//     sheet, and in page memory no sheet covers.
#include "client/palettesheets.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <set>

using namespace tak::render::palette;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
// The pre-M packer from GameView::preparePaletteTexture, verbatim apart from
// the GPU calls: pages may be added while `allowPage(pages)` says so.
struct LegacyPage { std::vector<Rect> free{{0, 0, 2048, 2048}}; };
struct LegacyPlacement { int page = -1; Rect rect; };
template <class Allow>
LegacyPlacement legacyPlace(std::vector<LegacyPage>& pages, int w, int h, Allow allowPage) {
    constexpr int side = 2048;
    if (w <= 0 || h <= 0 || w * 4 > side || h * 5 > side) return {};
    LegacyPage* page = nullptr;
    size_t freeIndex = 0;
    int bestArea = std::numeric_limits<int>::max();
    for (auto& candidate : pages) for (size_t i = 0; i < candidate.free.size(); ++i) {
        const auto& rect = candidate.free[i];
        if (rect.w >= w * 4 && rect.h >= h * 5 && rect.w * rect.h < bestArea) {
            page = &candidate; freeIndex = i; bestArea = rect.w * rect.h;
        }
    }
    if (!page) {
        if (!allowPage(pages.size())) return {};
        pages.push_back({}); page = &pages.back(); freeIndex = 0;
    }
    const auto available = page->free[freeIndex];
    Rect rect{available.x, available.y, w * 4, h * 5};
    page->free.erase(page->free.begin() + std::ptrdiff_t(freeIndex));
    if (available.w > rect.w) page->free.push_back({rect.x + rect.w, rect.y, available.w - rect.w, rect.h});
    if (available.h > rect.h) page->free.push_back({rect.x, rect.y + rect.h, available.w, available.h - rect.h});
    return {int(page - pages.data()), rect};
}
template <class Allow>
LegacyPlacement newPlace(PageLayout& layout, int w, int h, Allow allowPage) {
    if (!PageLayout::fitsPage(w, h)) return {};
    auto slot = layout.find(w, h);
    if (!slot) {
        if (!allowPage(layout.pages())) return {};
        slot = layout.addPage();
    }
    const Rect rect = layout.commit(*slot, w, h);
    return {int(slot->page), rect};
}
bool overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
}  // namespace

int main() {
    std::mt19937 rng(20261017);
    // ---- 1 + 2: placements, alignment, index rectangles ----
    for (int round = 0; round < 40; ++round) {
        std::vector<LegacyPage> legacy;
        PageLayout layout;
        const size_t cap = round % 4 == 0 ? 2 : kMaxPages;
        auto allow = [&](size_t pages) { return pages < cap; };
        std::vector<std::vector<Rect>> sheets(kMaxPages);
        const int frames = 200 + int(rng() % 700);
        for (int f = 0; f < frames; ++f) {
            // Shipped textures are 8..256 px; include degenerate and oversized ones.
            static const int sizes[] = {0, 1, 3, 8, 16, 24, 32, 48, 64, 96, 128, 200, 256, 409, 410, 512, 513};
            const int w = rng() % 9 == 0 ? sizes[rng() % 17] : 8 << (rng() % 5);
            const int h = rng() % 9 == 0 ? sizes[rng() % 17] : 8 << (rng() % 5);
            const auto a = legacyPlace(legacy, w, h, allow);
            const auto b = newPlace(layout, w, h, allow);
            CHECK(a.page == b.page);
            CHECK(a.rect.x == b.rect.x && a.rect.y == b.rect.y && a.rect.w == b.rect.w && a.rect.h == b.rect.h);
            if (b.page < 0) continue;
            CHECK(b.rect.x % kBandColumns == 0 && b.rect.y % kBandRows == 0);
            CHECK(b.rect.x + b.rect.w <= kPageSide && b.rect.y + b.rect.h <= kPageSide);
            const Rect index{b.rect.x / kBandColumns, b.rect.y / kBandRows, w, h};
            CHECK(index.x + index.w <= kIndexPageSide && index.y + index.h <= kTableRow);
            for (const Rect& other : sheets[size_t(b.page)]) CHECK(!overlap(other, index));
            sheets[size_t(b.page)].push_back(index);
        }
        CHECK(legacy.size() == layout.pages());
    }
    // ---- 3: sheet id in texture coordinates ----
    {
        size_t checked = 0;
        std::vector<uint32_t> ids;
        for (uint32_t s = 0; s < 64; ++s) ids.push_back(s);
        for (uint32_t s : {4031u, 4094u, 4095u, 4096u, 4097u, 8191u, 8192u, kMaxSheetsPerPage - 1}) ids.push_back(s);
        for (int r = 0; r < 64; ++r) ids.push_back(rng() % kMaxSheetsPerPage);
        for (uint32_t s : ids)
            for (int k = 0; k <= kPageSide; ++k)
                for (int kv : {0, k, kPageSide - k, int(rng() % (kPageSide + 1))}) {
                    const float u0 = float(k) / kPageSide, v0 = float(kv) / kPageSide;
                    float u = u0, v = v0;
                    encodeSheet(u, v, s);
                    uint32_t back = 0;
                    decodeSheet(u, v, back);
                    CHECK(back == s);
                    CHECK(std::memcmp(&u, &u0, sizeof u) == 0 && std::memcmp(&v, &v0, sizeof v) == 0);
                    ++checked;
                }
        // The baked expression itself is exact: origin/2048 + (band + corner) * w/2048.
        for (int r = 0; r < 100000; ++r) {
            const int w = 1 + int(rng() % 512), x = kBandColumns * int(rng() % (kPageSide / kBandColumns - w + 1));
            const int band = int(rng() % kBandColumns), corner = int(rng() % 2);
            if (x + (band + corner) * w > kPageSide) continue;
            const float baked = float(x) / kPageSide + (float(band) + float(corner)) * (float(w) / kPageSide);
            CHECK(baked == float(x + (band + corner) * w) / kPageSide);
        }
        std::printf("sheet coordinates: %zu exact round trips\n", checked);
    }
    // ---- 4: table entries ----
    for (int r = 0; r < 20000; ++r) {
        const SheetEntry e{int(rng() % 512), int(rng() % 410), 1 + int(rng() % 512), 1 + int(rng() % 409),
                           int(rng() % kMaxBanks)};
        const SheetEntry d = decodeEntry(encodeEntry(e));
        CHECK(d.x0q == e.x0q && d.y0q == e.y0q && d.w == e.w && d.h == e.h && d.bank == e.bank);
    }
    {
        std::set<std::pair<int, int>> texels;
        for (uint32_t s = 0; s < kMaxSheetsPerPage; ++s) {
            const Rect t = tableTexels(s);
            CHECK(t.x + t.w <= kIndexPageSide && t.y >= kTableRow && t.y < kIndexPageSide);
            for (int k = 0; k < t.w; ++k) CHECK(texels.insert({t.x + k, t.y}).second);
        }
    }
    // ---- 5: decode == baked expansion ----
    {
        std::vector<Bank> banks(3);
        for (auto& bank : banks) {
            for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) bank.palette[i][c] = uint8_t(rng());
            bank.shades.resize(kShadeTableBytes);
            for (auto& s : bank.shades) s = uint8_t(rng());
        }
        size_t texels = 0, outside = 0;
        std::vector<uint8_t> baked;
        PageLayout layout;
        for (int f = 0; f < 120; ++f) {
            Frame frame;
            frame.width = 1 + int(rng() % 70);
            frame.height = 1 + int(rng() % 70);
            frame.la.resize(size_t(frame.width) * frame.height * 2);
            for (size_t i = 0; i < frame.la.size(); ++i) frame.la[i] = i % 2 && rng() % 3 ? 255 : uint8_t(rng());
            const Bank& bank = banks[size_t(f) % banks.size()];
            auto slot = layout.find(frame.width, frame.height);
            if (!slot) slot = layout.addPage();
            const Rect rect = layout.commit(*slot, frame.width, frame.height);
            const SheetEntry e = entryFor(rect, frame.width, frame.height, int(f % 3));
            expandSheet(frame, bank, baked);
            for (int y = 0; y < rect.h; ++y)
                for (int x = 0; x < rect.w; ++x) {
                    const auto got = resolveTexel(e, frame, bank, rect.x + x, rect.y + y);
                    CHECK(decodeTexel(e, rect.x + x, rect.y + y).inside);
                    CHECK(std::memcmp(got.data(), baked.data() + (size_t(y) * rect.w + x) * 4, 4) == 0);
                    ++texels;
                }
            // Outside the sheet: the edge texel of the nearest band.
            for (auto [dx, dy] : {std::pair{-1, 0}, {rect.w, 0}, {0, -1}, {0, rect.h}, {-3, -2}, {rect.w + 2, rect.h + 5}}) {
                const Decoded d = decodeTexel(e, rect.x + dx, rect.y + dy);
                CHECK(!d.inside);
                CHECK(d.ix >= 0 && d.ix < frame.width && d.iy >= 0 && d.iy < frame.height);
                CHECK(d.level >= kFirstLevel && d.level < kFirstLevel + kBands);
                ++outside;
            }
        }
        // The LUT is the same table.
        static uint8_t lut[kShadeLevels][256][4];
        bankLut(banks[1], lut);
        for (int level = 0; level < kShadeLevels; ++level)
            for (int index = 0; index < 256; ++index) {
                const uint8_t m = banks[1].shades[size_t(level) * 256 + size_t(index)];
                CHECK(std::memcmp(lut[level][index], banks[1].palette[m], 3) == 0 && lut[level][index][3] == 255);
            }
        std::printf("decode: %zu sheet texels equal the baked expansion, %zu outside clamped\n", texels, outside);
    }
    // ---- 6: whole pages, neighbours included ----
    {
        std::vector<Bank> banks(kMaxBanks);
        for (auto& bank : banks) {
            for (int i = 0; i < 256; ++i) for (int c = 0; c < 4; ++c) bank.palette[i][c] = uint8_t(rng());
            bank.shades.resize(kShadeTableBytes);
            for (auto& s : bank.shades) s = uint8_t(rng());
        }
        PageLayout layout;
        std::vector<std::vector<uint8_t>> baked;
        std::vector<IndexPageModel> models;
        std::vector<std::vector<uint32_t>> ids;
        std::vector<uint8_t> expanded;
        for (int f = 0; f < 900; ++f) {
            Frame frame;
            static const int sizes[] = {1, 2, 3, 5, 8, 13, 16, 31, 32, 47, 64, 80, 128};
            frame.width = sizes[rng() % 13];
            frame.height = sizes[rng() % 13];
            frame.la.resize(size_t(frame.width) * frame.height * 2);
            for (size_t i = 0; i < frame.la.size(); i += 2) {
                frame.la[i] = uint8_t(rng());
                frame.la[i + 1] = rng() % 4 ? 255 : uint8_t(rng());
            }
            auto slot = layout.find(frame.width, frame.height);
            if (!slot) {
                if (layout.pages() == 2) continue;   // two pages: full ones and a partly used one
                slot = layout.addPage();
                baked.emplace_back(size_t(kPageSide) * kPageSide * 4, 0);   // never-written memory reads zero
                models.emplace_back();
                ids.emplace_back();
            }
            const Rect rect = layout.commit(*slot, frame.width, frame.height);
            const int bank = int(rng() % kMaxBanks);
            const uint32_t id = uint32_t(ids[slot->page].size());
            expandSheet(frame, banks[size_t(bank)], expanded);
            for (int y = 0; y < rect.h; ++y)
                std::memcpy(baked[slot->page].data() + (size_t(rect.y + y) * kPageSide + size_t(rect.x)) * 4,
                            expanded.data() + size_t(y) * rect.w * 4, size_t(rect.w) * 4);
            models[slot->page].place(frame, rect, id, bank);
            ids[slot->page].push_back(id);
        }
        size_t own = 0, neighbour = 0, empty = 0, mismatches = 0;
        for (size_t p = 0; p < models.size(); ++p)
            for (int Y = 0; Y < kPageSide; ++Y)
                for (int X = 0; X < kPageSide; ++X) {
                    const uint32_t o = models[p].owner(X, Y);
                    // The sampling polygon: the texel's own sheet half the time, else any sheet of the page.
                    const uint32_t sheet = o && rng() % 2 ? o - 1 : ids[p][rng() % ids[p].size()];
                    bool outside = false;
                    const auto got = models[p].resolve(sheet, X, Y, banks, &outside);
                    if (outside && o) CHECK(decodeTexel(models[p].entry(o - 1), X, Y).inside);
                    if (std::memcmp(got.data(), baked[p].data() + (size_t(Y) * kPageSide + size_t(X)) * 4, 4) != 0 &&
                        ++mismatches <= 8)
                        std::printf("  page %zu texel %d,%d sheet %u owner %u\n", p, X, Y, sheet, o);
                    (outside ? (o ? neighbour : empty) : own)++;
                }
        CHECK(mismatches == 0);
        CHECK(neighbour > 0 && empty > 0);
        std::printf("pages: %zu texels on %zu pages equal the baked pages (%zu own sheet, %zu neighbouring sheet, %zu unused)\n",
                    own + neighbour + empty, models.size(), own, neighbour, empty);
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("PASS: palette sheets\n");
    return 0;
}
