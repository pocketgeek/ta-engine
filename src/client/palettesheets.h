#pragma once

// Structure palette lighting: page layout, CPU storage and the indexed form
// (workstream M).
//
// Retail lights a structure's model textures per polygon through the faction
// shade table (palettes/<bank>_textures.shd): colour = palette[shade[level][index]]
// for the polygon's light level 5..24. The renderer has always drawn that as
// "sheets": each texture frame expanded into its 20 reachable shade bands, laid
// out four bands across and five down (a 4w x 5h RGBA rectangle), packed into
// 2048x2048 pages; a polygon picks its band through its texture coordinates.
// That costs 80 bytes per source texel (20 bands x RGBA), 144 MiB of pages for
// the shipped structure roster.
//
// This header keeps the layout and gives it a second, compact form:
//
//   * PageLayout is the former page packer, unchanged (best-area free rectangle,
//     guillotine split, same order), but it only computes placements. The baked
//     pages and the indexed pages both use it, so a polygon's texture
//     coordinates are the same numbers in both forms and the texel either form
//     samples is the same texel.
//   * The indexed form stores each frame ONCE in a 512x512 RGBA page: sheet
//     (x0, y0) goes to (x0/4, y0/5), each texel (palette index, owner sheet,
//     alpha). The packer only ever advances x by multiples of 4 (sheet widths
//     are 4w) and y by multiples of 5 (heights 5h), so these are exact and two
//     sheets never overlap; rows [0, 410) hold index data, rows [kTableRow, 512)
//     a sheet table. The same arithmetic maps ANY baked page texel (X, Y) to
//     index texel (X/4, Y/5), and that texel's owner names the sheet whose baked
//     rectangle holds (X, Y) -- so a sample outside the polygon's own sheet is
//     resolved exactly like the baked page's neighbouring texel (IndexPageModel).
//   * A polygon names its sheet through the integer part of its texture
//     coordinates (encodeSheetCoord): the baked coordinates are exact multiples
//     of 1/2048 in [0, 1], so u + 2*s is exact in float for s < 4096 and the
//     shader recovers both the original u and s exactly.
//   * resolveTexel is the decode the indexed shader performs inside a sheet, on
//     the CPU; IndexPageModel::resolve is the whole decode, neighbours included:
//     at every texel of a page it returns exactly the baked page's texel
//     (tools/palette_sheets_test.cpp checks every texel of random pages).
//
// Pure: no SDL, no GL. GameView owns the instances (render thread only).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace tak::render::palette {

inline constexpr int kPageSide = 2048;           // baked page (and layout) size
inline constexpr size_t kMaxPages = 16;          // 256 MiB of baked pages; the roster uses nine
inline constexpr int kBands = 20;                // reachable shade levels 5..24
inline constexpr int kFirstLevel = 5;
inline constexpr int kBandColumns = 4, kBandRows = 5;
inline constexpr int kShadeLevels = 32;          // rows of a .shd table
inline constexpr size_t kShadeTableBytes = size_t(kShadeLevels) * 256;

// Indexed page: index data at (x0/4, y0/5), sheet table above it. RGBA texels:
// R = palette index, G/B = owner (sheet id + 1, low byte in G; 0 = no sheet),
// A = alpha. Table texels use R and A only.
inline constexpr int kIndexPageSide = 512;
inline constexpr size_t kIndexTexelBytes = 4;
inline constexpr int kTableRow = 410;            // y0/5 + h <= 2048/5 < 410
inline constexpr int kTableTexelsPerSheet = 4;
inline constexpr uint32_t kMaxSheetsPerPage =
    uint32_t((kIndexPageSide - kTableRow) * kIndexPageSide / kTableTexelsPerSheet);
inline constexpr size_t kIndexPageBytes = size_t(kIndexPageSide) * kIndexPageSide * kIndexTexelBytes;
inline constexpr size_t kBakedPageBytes = size_t(kPageSide) * kPageSide * 4;
inline constexpr int kMaxBanks = 8;              // LUT rows: kMaxBanks * kShadeLevels
inline constexpr uint32_t kSheetCoordBase = 4096;   // s = su + 4096 * sv
inline constexpr uint32_t kNoSheet = std::numeric_limits<uint32_t>::max();

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// One faction texture bank: its palette and shade table, shared by every
// texture of the bank (before M each texture held its own copy of both).
struct Bank {
    uint8_t palette[256][4]{};
    std::vector<uint8_t> shades;   // kShadeTableBytes
};

// A source frame in indexed form: (index, alpha) per texel, row-major.
struct Frame {
    int width = 0, height = 0;
    std::vector<uint8_t> la;
};

// The former page packer (GameView::preparePaletteTexture before M), as a
// layout only. find() names the best existing free rectangle; when none fits
// the caller decides whether a page may be added (the baked form checks GPU
// budget there) and calls addPage(); commit() splits the rectangle exactly as
// before. A placement found but not committed leaves the layout unchanged.
class PageLayout {
public:
    struct Slot { size_t page = 0, freeIndex = 0; };
    static bool fitsPage(int w, int h) {
        return w > 0 && h > 0 && w * kBandColumns <= kPageSide && h * kBandRows <= kPageSide;
    }
    std::optional<Slot> find(int w, int h) const {
        std::optional<Slot> best;
        int bestArea = std::numeric_limits<int>::max();
        for (size_t p = 0; p < pages_.size(); ++p)
            for (size_t i = 0; i < pages_[p].size(); ++i) {
                const Rect& r = pages_[p][i];
                if (r.w >= w * kBandColumns && r.h >= h * kBandRows && r.w * r.h < bestArea) {
                    best = Slot{p, i};
                    bestArea = r.w * r.h;
                }
            }
        return best;
    }
    size_t pages() const { return pages_.size(); }
    Slot addPage() {
        pages_.push_back({Rect{0, 0, kPageSide, kPageSide}});
        return Slot{pages_.size() - 1, 0};
    }
    // The sheet rectangle (4w x 5h) a slot would receive.
    Rect sheetRect(Slot slot, int w, int h) const {
        const Rect& a = pages_[slot.page][slot.freeIndex];
        return Rect{a.x, a.y, w * kBandColumns, h * kBandRows};
    }
    Rect commit(Slot slot, int w, int h) {
        auto& free = pages_[slot.page];
        const Rect available = free[slot.freeIndex];
        const Rect rect{available.x, available.y, w * kBandColumns, h * kBandRows};
        free.erase(free.begin() + std::ptrdiff_t(slot.freeIndex));
        if (available.w > rect.w) free.push_back({rect.x + rect.w, rect.y, available.w - rect.w, rect.h});
        if (available.h > rect.h) free.push_back({rect.x, rect.y + rect.h, available.w, available.h - rect.h});
        return rect;
    }
    void clear() { pages_.clear(); }
    const std::vector<Rect>& freeRects(size_t page) const { return pages_[page]; }
private:
    std::vector<std::vector<Rect>> pages_;
};

// The baked form of one sheet: 20 bands of RGBA, band b = level - 5 at column
// b % 4, row b / 4 -- exactly the pre-M expansion loop.
inline void expandSheet(const Frame& frame, const Bank& bank, std::vector<uint8_t>& out) {
    const int w = frame.width, h = frame.height;
    out.resize(size_t(w) * h * kBands * 4);
    for (int level = 0; level < kBands; ++level)
        for (size_t pixel = 0; pixel < size_t(w) * h; ++pixel) {
            const uint8_t index = frame.la[pixel * 2];
            const uint8_t remapped = bank.shades[size_t(level + kFirstLevel) * 256 + index];
            const size_t row = size_t(level / kBandColumns) * h + pixel / size_t(w);
            const size_t col = size_t(level % kBandColumns) * w + pixel % size_t(w);
            uint8_t* output = out.data() + (row * size_t(w) * kBandColumns + col) * 4;
            std::memcpy(output, bank.palette[remapped], 3);
            output[3] = frame.la[pixel * 2 + 1];
        }
}

// LUT row block of one bank: RGBA for (level, index), levels 0..31.
inline void bankLut(const Bank& bank, uint8_t out[kShadeLevels][256][4]) {
    for (int level = 0; level < kShadeLevels; ++level)
        for (int index = 0; index < 256; ++index) {
            const uint8_t remapped = bank.shades[size_t(level) * 256 + size_t(index)];
            std::memcpy(out[level][index], bank.palette[remapped], 3);
            out[level][index][3] = 255;
        }
}

// ---- sheet table (indexed page rows [kTableRow, 512)) ------------------------
// Four texels per sheet, each a 16-bit value low byte in R, high byte in A
// (encodeEntry gives the (R, A) pairs): x0/4, y0/5, w, then h with the bank in
// A's upper bits (h <= 409 needs one high bit).
struct SheetEntry { int x0q = 0, y0q = 0, w = 0, h = 0, bank = 0; };

inline std::array<uint8_t, 8> encodeEntry(const SheetEntry& e) {
    return {uint8_t(e.x0q & 255), uint8_t(e.x0q >> 8), uint8_t(e.y0q & 255), uint8_t(e.y0q >> 8),
            uint8_t(e.w & 255),   uint8_t(e.w >> 8),   uint8_t(e.h & 255),
            uint8_t((e.h >> 8) | (e.bank << 1))};
}
inline SheetEntry decodeEntry(const std::array<uint8_t, 8>& t) {
    return {t[0] | (t[1] << 8), t[2] | (t[3] << 8), t[4] | (t[5] << 8), t[6] | ((t[7] & 1) << 8), t[7] >> 1};
}
// Texel (column, row) of entry texel k of sheet s in the indexed page.
inline Rect tableTexels(uint32_t sheet) {
    const uint32_t base = sheet * kTableTexelsPerSheet;
    return Rect{int(base % kIndexPageSide), kTableRow + int(base / kIndexPageSide), kTableTexelsPerSheet, 1};
}
inline SheetEntry entryFor(const Rect& sheetRect, int w, int h, int bank) {
    return SheetEntry{sheetRect.x / kBandColumns, sheetRect.y / kBandRows, w, h, bank};
}
// The entry's four RGBA table texels.
inline std::array<uint8_t, kTableTexelsPerSheet * kIndexTexelBytes> entryTexels(const SheetEntry& e) {
    const auto pairs = encodeEntry(e);
    std::array<uint8_t, kTableTexelsPerSheet * kIndexTexelBytes> out{};
    for (int k = 0; k < kTableTexelsPerSheet; ++k) {
        out[size_t(k) * 4] = pairs[size_t(k) * 2];
        out[size_t(k) * 4 + 3] = pairs[size_t(k) * 2 + 1];
    }
    return out;
}
// One frame's index texels (R index, G/B owner = sheet + 1, A alpha), row-major.
inline void indexTexels(const Frame& frame, uint32_t sheet, std::vector<uint8_t>& out) {
    const size_t texels = size_t(frame.width) * size_t(frame.height);
    const uint32_t owner = sheet + 1;
    out.resize(texels * kIndexTexelBytes);
    for (size_t i = 0; i < texels; ++i) {
        uint8_t* t = out.data() + i * kIndexTexelBytes;
        t[0] = frame.la[i * 2];
        t[1] = uint8_t(owner & 255);
        t[2] = uint8_t(owner >> 8);
        t[3] = frame.la[i * 2 + 1];
    }
}

// ---- sheet id in texture coordinates ---------------------------------------
// A baked coordinate c is k/2048 with 0 <= k <= 2048 (sheet origin and band
// offsets are whole texels): 11 fraction bits. c + 2*n with n < 4096 needs at
// most 13 integer bits, so it is exact in float, and so is the shader's
// n = floor(c'/2), c = c' - 2n. The interpolated c therefore equals SDL's.
inline float encodeSheetCoord(float c, uint32_t n) { return c + 2.0f * float(n); }
inline void encodeSheet(float& u, float& v, uint32_t sheet) {
    u = encodeSheetCoord(u, sheet % kSheetCoordBase);
    v = encodeSheetCoord(v, sheet / kSheetCoordBase);
}
inline void decodeSheet(float& u, float& v, uint32_t& sheet) {
    const float su = std::floor(u * 0.5f), sv = std::floor(v * 0.5f);
    u -= 2.0f * su; v -= 2.0f * sv;
    sheet = uint32_t(su) + kSheetCoordBase * uint32_t(sv);
}

// ---- the shader's decode, on the CPU ----------------------------------------
// The band of sheet e containing baked-page texel (X, Y) (clamped to the sheet;
// `inside` false when (X, Y) is outside it -- the shader then decodes with the
// owner's entry instead, IndexPageModel::resolve), the texel within the band,
// and colour = LUT[bank][5 + band][index], alpha from the index page.
struct Decoded { int ix = 0, iy = 0, level = 0; bool inside = true; };
inline Decoded decodeTexel(const SheetEntry& e, int X, int Y) {
    const int dx = X - e.x0q * kBandColumns, dy = Y - e.y0q * kBandRows;
    int col = dx >= 0 ? dx / e.w : -1, row = dy >= 0 ? dy / e.h : -1;
    Decoded d;
    d.inside = col >= 0 && col < kBandColumns && row >= 0 && row < kBandRows;
    col = std::clamp(col, 0, kBandColumns - 1);
    row = std::clamp(row, 0, kBandRows - 1);
    d.ix = std::clamp(dx - col * e.w, 0, e.w - 1);
    d.iy = std::clamp(dy - row * e.h, 0, e.h - 1);
    d.level = kFirstLevel + row * kBandColumns + col;
    return d;
}
inline std::array<uint8_t, 4> resolveTexel(const SheetEntry& e, const Frame& frame, const Bank& bank,
                                           int X, int Y) {
    const Decoded d = decodeTexel(e, X, Y);
    const size_t texel = size_t(d.iy) * size_t(frame.width) + size_t(d.ix);
    const uint8_t index = frame.la[texel * 2];
    const uint8_t remapped = bank.shades[size_t(d.level) * 256 + index];
    return {bank.palette[remapped][0], bank.palette[remapped][1], bank.palette[remapped][2],
            frame.la[texel * 2 + 1]};
}

// The indexed page as the shader reads it, on the CPU (tests): the RGBA texels
// and the decode -- own sheet's entry; for a texel outside it, the owner named
// at (X/4, Y/5) and ITS entry (the baked page's neighbouring sheet); no owner:
// transparent black (page memory no sheet was written to).
class IndexPageModel {
public:
    IndexPageModel() : texels_(kIndexPageBytes, 0) {}
    void place(const Frame& frame, const Rect& sheetRect, uint32_t sheet, int bank) {
        std::vector<uint8_t> t;
        indexTexels(frame, sheet, t);
        const int x0 = sheetRect.x / kBandColumns, y0 = sheetRect.y / kBandRows;
        for (int y = 0; y < frame.height; ++y)
            std::memcpy(at(x0, y0 + y), t.data() + size_t(y) * frame.width * kIndexTexelBytes,
                        size_t(frame.width) * kIndexTexelBytes);
        const auto table = entryTexels(entryFor(sheetRect, frame.width, frame.height, bank));
        const Rect r = tableTexels(sheet);
        std::memcpy(at(r.x, r.y), table.data(), table.size());
    }
    SheetEntry entry(uint32_t sheet) const {
        const Rect r = tableTexels(sheet);
        std::array<uint8_t, 8> pairs{};
        for (int k = 0; k < kTableTexelsPerSheet; ++k) {
            pairs[size_t(k) * 2] = at(r.x + k, r.y)[0];
            pairs[size_t(k) * 2 + 1] = at(r.x + k, r.y)[3];
        }
        return decodeEntry(pairs);
    }
    // Owner of baked texel (X, Y): sheet id + 1, 0 = none.
    uint32_t owner(int X, int Y) const {
        const uint8_t* t = at(X / kBandColumns, Y / kBandRows);
        return uint32_t(t[1]) | (uint32_t(t[2]) << 8);
    }
    // What the NEAREST program shows for baked texel (X, Y) sampled by a polygon
    // of `sheet` (banks: LUT rows).
    std::array<uint8_t, 4> resolve(uint32_t sheet, int X, int Y, const std::vector<Bank>& banks,
                                   bool* outside = nullptr) const {
        SheetEntry e = entry(sheet);
        Decoded d = decodeTexel(e, X, Y);
        if (outside) *outside = !d.inside;
        if (!d.inside) {
            const uint32_t o = owner(X, Y);
            if (!o) return {0, 0, 0, 0};
            e = entry(o - 1);
            d = decodeTexel(e, X, Y);
        }
        const uint8_t* t = at(e.x0q + d.ix, e.y0q + d.iy);
        const Bank& bank = banks[size_t(e.bank)];
        const uint8_t remapped = bank.shades[size_t(d.level) * 256 + t[0]];
        return {bank.palette[remapped][0], bank.palette[remapped][1], bank.palette[remapped][2], t[3]};
    }
    const std::vector<uint8_t>& texels() const { return texels_; }
private:
    uint8_t* at(int x, int y) { return texels_.data() + (size_t(y) * kIndexPageSide + size_t(x)) * kIndexTexelBytes; }
    const uint8_t* at(int x, int y) const {
        return texels_.data() + (size_t(y) * kIndexPageSide + size_t(x)) * kIndexTexelBytes;
    }
    std::vector<uint8_t> texels_;
};

}  // namespace tak::render::palette
