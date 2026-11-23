// gaf_rle_test — the GAF frame decoder on synthetic buffers: valid transparent/repeat/
// literal runs decode to exact pixels, and every malformed run is rejected with the
// parser's runtime_error before any byte outside its scanline (or the buffer) is read.
// Composite frames: a valid one still composes, while a subframe that is a composite
// itself (or points back at its parent) is refused instead of recursing until the stack
// runs out, and a file asking for more pixels or frames than the decode budget is refused
// before the memory is allocated. Feature GAFs arrive in peers' map packages, so this is
// untrusted input.

#include "gaf/gaf.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kTransparent = 9;

void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

// One-entry, one-frame GAF whose RLE data (dataPtr 88) is `rle`, with nothing after it
// unless the caller appends bytes.
std::vector<uint8_t> rleGaf(uint16_t w, uint16_t h, const std::vector<uint8_t>& rle) {
    std::vector<uint8_t> d(88);
    auto u16 = [&](size_t at, uint16_t v) { d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); };
    auto u32 = [&](size_t at, uint32_t v) { u16(at, uint16_t(v)); u16(at + 2, uint16_t(v >> 16)); };
    u32(0, 0x00010100); u32(4, 1); u32(12, 16);      // version, 1 entry at 16
    u16(16, 1); std::memcpy(&d[24], "rle", 3);       // 1 frame, name
    u32(56, 64); u32(60, 2);                         // frame record at 64, delay 2
    u16(64, w); u16(66, h);
    d[72] = kTransparent; d[73] = 1;                 // transparency index, RLE encoding
    u32(80, 88);                                     // dataPtr
    d.insert(d.end(), rle.begin(), rle.end());
    return {d.begin(), d.end()};  // exact capacity, so ASan sees any read past the end
}

tak::gaf::Palette palette() {
    std::vector<uint8_t> raw(1024);
    for (int i = 0; i < 256; ++i) {
        raw[size_t(i) * 4] = uint8_t(i);
        raw[size_t(i) * 4 + 1] = uint8_t(255 - i);
        raw[size_t(i) * 4 + 2] = uint8_t(i ^ 0x5A);
    }
    return tak::gaf::Palette::fromBytes(raw);
}

void expectRejected(const std::vector<uint8_t>& gaf, const char* what) {
    try {
        tak::gaf::load(gaf, palette(), -1, what);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(std::string("malformed RLE accepted: ") + what);
}

void validRuns() {
    // 8x4 frame. Codes: odd = skip code>>1; bit 1 = repeat (code>>2)+1 of the next byte;
    // else literal (code>>2)+1 bytes. Runs past the width are clipped, not rejected.
    const std::vector<uint8_t> rle = {
        6, 0,  0x05, 0x0A, 7, 0x04, 1, kTransparent,  // skip 2, 3x colour 7, literal {1, transparent}
        5, 0,  0x00, 200, 0x06, kTransparent, 0x0B,   // literal {200}, 2x transparent, skip 5
        0, 0,                                         // empty line
        7, 0,  0x0B, 0x01, 0x0C, 30, 31, 32, 33,      // skip 5, skip 0, literal 4 from x=5 (1 clipped)
    };
    const int expected[4][8] = {
        {-1, -1, 7, 7, 7, 1, -1, -1},
        {200, -1, -1, -1, -1, -1, -1, -1},
        {-1, -1, -1, -1, -1, -1, -1, -1},
        {-1, -1, -1, -1, -1, 30, 31, 32},
    };
    const auto pal = palette();
    const auto seqs = tak::gaf::load(rleGaf(8, 4, rle), pal, -1, "valid");
    check(seqs.size() == 1 && seqs[0].frames.size() == 1, "valid: one sequence, one frame");
    const auto& fr = seqs[0].frames[0];
    check(fr.width == 8 && fr.height == 4 && fr.rgba.size() == 8 * 4 * 4, "valid: frame size");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint8_t* px = &fr.rgba[(size_t(y) * 8 + x) * 4];
            const int idx = expected[y][x];
            const uint8_t want[4] = {idx < 0 ? uint8_t(0) : pal.rgba[idx][0],
                                     idx < 0 ? uint8_t(0) : pal.rgba[idx][1],
                                     idx < 0 ? uint8_t(0) : pal.rgba[idx][2],
                                     uint8_t(idx < 0 ? 0 : 255)};
            check(std::memcmp(px, want, 4) == 0,
                  "valid: pixel (" + std::to_string(x) + "," + std::to_string(y) + ") differs");
        }

    // A run ending exactly on the scanline's last byte with bytes after it (next line).
    const auto twoLines = tak::gaf::load(rleGaf(2, 2, {2, 0, 0x06, 4, 2, 0, 0x06, 5, 0xEE}), pal);
    const auto& t = twoLines[0].frames[0].rgba;
    check(t[0] == 4 && t[4] == 4 && t[8] == 5 && t[12] == 5 && t[15] == 255,
          "valid: repeat runs that end on their scanline boundary");
}

void malformedRuns() {
    // Operands missing at the very end of the buffer (the scanline is declared whole).
    expectRejected(rleGaf(4, 1, {1, 0, 0x02}), "repeat without its colour byte");
    expectRejected(rleGaf(4, 1, {3, 0, 0x0C, 1, 2}), "literal of 4 with 2 bytes");
    expectRejected(rleGaf(4, 1, {2, 0, 0x01, 0x00}), "literal of 1 without its byte");

    // The operand bytes exist in the file but belong to the next scanline.
    expectRejected(rleGaf(4, 2, {1, 0, 0x02, 2, 0, 0x02, 3}), "repeat crossing its scanline");
    expectRejected(rleGaf(4, 2, {2, 0, 0x08, 5, 2, 0, 0x02, 3}), "literal crossing its scanline");
    expectRejected(rleGaf(4, 1, {2, 0, 0x08, 5, 6, 7, 8, 9}), "literal crossing the last scanline");

    // Truncated scanline headers and declared scanlines.
    expectRejected(rleGaf(4, 2, {2, 0, 0x02, 3, 4}), "half a scanline header");
    expectRejected(rleGaf(4, 2, {2, 0, 0x02, 3}), "missing scanline header");
    expectRejected(rleGaf(4, 1, {10, 0, 0x02, 3}), "declared scanline longer than the file");
    expectRejected(rleGaf(4, 1, {0xFF, 0xFF}), "65535-byte scanline in a short file");
}

// A one-entry GAF with `records` frame records (each naming `frameOff`, delay 2), then
// whatever the caller writes at the offsets it returns from put(). Entries are split at
// 65535 records (the per-entry count is 16-bit).
struct Builder {
    std::vector<uint8_t> d;
    void u16(size_t at, uint16_t v) { grow(at + 2); d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); }
    void u32(size_t at, uint32_t v) { u16(at, uint16_t(v)); u16(at + 2, uint16_t(v >> 16)); }
    void grow(size_t n) { if (d.size() < n) d.resize(n); }
    // A 24-byte frame header at the end of the buffer; its offset.
    uint32_t frame(uint16_t w, uint16_t h, int16_t xo, int16_t yo, uint8_t enc, uint8_t subs, uint32_t data) {
        const uint32_t at = uint32_t(d.size());
        grow(at + 24);
        u16(at, w); u16(at + 2, h); u16(at + 4, uint16_t(xo)); u16(at + 6, uint16_t(yo));
        d[at + 8] = kTransparent; d[at + 9] = enc; d[at + 10] = subs;
        u32(at + 16, data);
        return at;
    }
    uint32_t bytes(const std::vector<uint8_t>& b) {
        const uint32_t at = uint32_t(d.size());
        d.insert(d.end(), b.begin(), b.end());
        return at;
    }
    // The header and entry tables for `entries` entries of `perEntry` records each, all
    // pointing at whatever patch() later names; frames go after them.
    std::vector<size_t> table(uint32_t entries, uint16_t perEntry) {
        u32(0, 0x00010100); u32(4, entries);
        std::vector<size_t> records;
        size_t at = 12 + size_t(entries) * 4;
        for (uint32_t e = 0; e < entries; ++e) {
            u32(12 + e * 4, uint32_t(at));
            u16(at, perEntry);
            for (uint16_t f = 0; f < perEntry; ++f) { records.push_back(at + 40 + f * 8); u32(at + 40 + f * 8 + 4, 2); }
            at += 40 + size_t(perEntry) * 8;
        }
        grow(at);
        return records;
    }
    void point(const std::vector<size_t>& records, uint32_t frameOff) { for (size_t r : records) u32(r, frameOff); }
    std::vector<uint8_t> done() const { return {d.begin(), d.end()}; }
};

void expectRefused(const std::vector<uint8_t>& gaf, const char* what) {
    try {
        tak::gaf::load(gaf, palette(), -1, what);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(std::string("malformed GAF accepted: ") + what);
}

void compositeFrames() {
    // Valid: a 4x2 composite of two raw 2x2 subframes, anchored so they sit side by side.
    {
        Builder b;
        const auto rec = b.table(1, 1);
        const uint32_t left = b.bytes({1, 2, 3, 4}), right = b.bytes({5, 6, 7, kTransparent});
        const uint32_t s0 = b.frame(2, 2, 0, 0, 0, 0, left), s1 = b.frame(2, 2, -2, 0, 0, 0, right);
        const uint32_t ptrs = b.bytes({0, 0, 0, 0, 0, 0, 0, 0});
        b.u32(ptrs, s0); b.u32(ptrs + 4, s1);
        b.point(rec, b.frame(4, 2, 0, 0, 0, 2, ptrs));
        const auto pal = palette();
        const auto seqs = tak::gaf::load(b.done(), pal, -1, "composite");
        const auto& fr = seqs[0].frames[0];
        const int want[2][4] = {{1, 2, 5, 6}, {3, 4, 7, -1}};
        check(fr.width == 4 && fr.height == 2, "composite: frame size");
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x) {
                const uint8_t* px = &fr.rgba[(size_t(y) * 4 + x) * 4];
                const int idx = want[y][x];
                check(idx < 0 ? px[3] == 0 : (px[0] == pal.rgba[idx][0] && px[3] == 255),
                      "composite: pixel (" + std::to_string(x) + "," + std::to_string(y) + ") differs");
            }
    }
    // A composite whose subframe is itself: followed, it never ends.
    {
        Builder b;
        const auto rec = b.table(1, 1);
        const uint32_t ptr = b.bytes({0, 0, 0, 0});
        const uint32_t self = b.frame(2, 2, 0, 0, 0, 1, ptr);
        b.u32(ptr, self);
        b.point(rec, self);
        expectRefused(b.done(), "a composite that is its own subframe");
    }
    // A composite inside a composite (no shipped GAF nests).
    {
        Builder b;
        const auto rec = b.table(1, 1);
        const uint32_t leaf = b.frame(1, 1, 0, 0, 0, 0, b.bytes({3}));
        const uint32_t innerPtr = b.bytes({0, 0, 0, 0});
        b.u32(innerPtr, leaf);
        const uint32_t inner = b.frame(1, 1, 0, 0, 0, 1, innerPtr);
        const uint32_t outerPtr = b.bytes({0, 0, 0, 0});
        b.u32(outerPtr, inner);
        b.point(rec, b.frame(1, 1, 0, 0, 0, 1, outerPtr));
        expectRefused(b.done(), "a composite subframe");
    }
}

void decodeBudget() {
    // One frame declaring 65535 x 65535 (17 GB of rgba) in a few dozen bytes.
    {
        Builder b;
        const auto rec = b.table(1, 1);
        b.point(rec, b.frame(65535, 65535, 0, 0, 1, 0, 0));
        expectRefused(b.done(), "a 65535 x 65535 frame");
    }
    // 300 records sharing one 1024 x 1024 RLE frame of empty scanlines: 1.2 GB decoded
    // from a 5 KB file.
    {
        Builder b;
        const auto rec = b.table(1, 300);
        const uint32_t lines = b.bytes(std::vector<uint8_t>(2048, 0));
        b.point(rec, b.frame(1024, 1024, 0, 0, 1, 0, lines));
        expectRefused(b.done(), "300 shared 1024 x 1024 frames");
    }
    // 70,000 records of one empty frame: no pixels, but past the frame limit.
    {
        Builder b;
        const auto rec = b.table(2, 35000);
        b.point(rec, b.frame(0, 0, 0, 0, 0, 0, 0));
        expectRefused(b.done(), "70,000 frames");
    }
    // The largest shipped file's load (102 MB, anims/loading.gaf) still fits: 100 shared
    // 512 x 512 frames.
    {
        Builder b;
        const auto rec = b.table(1, 100);
        const uint32_t lines = b.bytes(std::vector<uint8_t>(1024, 0));
        b.point(rec, b.frame(512, 512, 0, 0, 1, 0, lines));
        check(tak::gaf::load(b.done(), palette())[0].frames.size() == 100, "budget: 100 MB of frames still load");
    }
}

} // namespace

int main() try {
    validRuns();
    malformedRuns();
    compositeFrames();
    decodeBudget();
    std::cout << "gaf_rle: ok\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "gaf_rle: FAIL: " << e.what() << "\n";
    return 1;
}
