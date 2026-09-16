// fnt_test -- TA's .FNT bitmap fonts.
//
// Data-independent, so CI runs it. The fonts it builds are hand-assembled from
// the format definition, which is what makes this a real check rather than a
// round trip against my own writer.
//
// The rule worth pinning is the packing: a glyph is width x height BITS, MSB
// first, row-major, and rows are NOT byte-aligned. Assuming byte-aligned rows
// (the obvious reading, and what most bitmap formats do) still decodes an
// 8-wide glyph perfectly -- every row lands on a byte boundary by coincidence --
// and garbles every other width. Most TA glyphs are 8 wide, so that mistake
// would look almost right.

#include "fnt/fnt.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-66s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// Build a .FNT holding the given glyphs. `rows` is one string per row, '#' set.
static std::vector<uint8_t> build(int height,
                                  const std::vector<std::pair<int, std::vector<std::string>>>& glyphs) {
    std::vector<uint8_t> d(4 + 256 * 2, 0);
    d[0] = uint8_t(height); d[1] = uint8_t(height >> 8);
    d[2] = 1;
    for (const auto& [ch, rows] : glyphs) {
        int w = rows.empty() ? 0 : int(rows[0].size());
        size_t off = d.size();
        d[4 + size_t(ch) * 2] = uint8_t(off);
        d[4 + size_t(ch) * 2 + 1] = uint8_t(off >> 8);
        d.push_back(uint8_t(w));
        // Pack width*height bits continuously, MSB first -- rows straddle bytes.
        std::vector<uint8_t> bits;
        for (int r = 0; r < height; ++r)
            for (int c = 0; c < w; ++c)
                bits.push_back(r < int(rows.size()) && c < int(rows[size_t(r)].size()) &&
                                       rows[size_t(r)][size_t(c)] == '#'
                                   ? 1 : 0);
        for (size_t i = 0; i < bits.size(); i += 8) {
            uint8_t b = 0;
            for (size_t k = 0; k < 8 && i + k < bits.size(); ++k)
                if (bits[i + k]) b |= uint8_t(1 << (7 - k));
            d.push_back(b);
        }
    }
    return d;
}

int main() {
    std::printf("fnt_test\n");

    // A 3-wide glyph over 4 rows = 12 bits, which does NOT fill whole bytes --
    // exactly the case a byte-aligned-rows reading gets wrong.
    {
        auto bytes = build(4, {{'L', {"#..", "#..", "#..", "###"}}});
        auto f = ta::fnt::parse(bytes, "<synthetic>");
        check(f.height == 4, "height reads");
        const auto& g = f.glyphs['L'];
        check(g.present && g.width == 3, "glyph width reads");
        auto px = [&](int r, int c) { return g.bits[size_t(r) * 3 + size_t(c)] != 0; };
        check(px(0,0) && !px(0,1) && !px(0,2), "row 0 unpacks");
        check(px(3,0) && px(3,1) && px(3,2),
              "the LAST row unpacks -- rows are not byte-aligned");
        check(!f.glyphs['Z'].present, "an undeclared character is absent, not blank");
    }

    // 8-wide: every row lands on a byte boundary. Both readings agree here, which
    // is why this case cannot be the only one tested.
    {
        auto bytes = build(3, {{'A', {"..####..", ".#....#.", "#......#"}}});
        auto f = ta::fnt::parse(bytes, "<synthetic>");
        const auto& g = f.glyphs['A'];
        check(g.present && g.width == 8, "an 8-wide glyph reads");
        check(g.bits[2] && g.bits[5] && !g.bits[0], "its first row unpacks");
        check(g.bits[2 * 8 + 0] && g.bits[2 * 8 + 7], "and its last row too");
    }

    // measure() sums declared widths and skips characters the font lacks.
    {
        auto bytes = build(4, {{'A', {"##", "##", "##", "##"}},
                               {'B', {"###", "###", "###", "###"}}});
        auto f = ta::fnt::parse(bytes, "<synthetic>");
        check(f.measure("AB") == 5, "measure sums glyph widths");
        check(f.measure("AZB") == 5, "measure skips absent characters");
        check(f.measure("") == 0, "measure of nothing is 0");
    }

    // blit clips instead of writing out of bounds.
    {
        auto bytes = build(2, {{'X', {"##", "##"}}});
        auto f = ta::fnt::parse(bytes, "<synthetic>");
        std::vector<uint8_t> buf(4 * 4, 0);
        f.blit("X", buf, 4, 4, 3, 3);          // only its top-left pixel fits
        check(buf[3 * 4 + 3] == 1, "the in-bounds pixel is drawn");
        int lit = 0;
        for (uint8_t b : buf) lit += b;
        check(lit == 1, "and the out-of-bounds ones are dropped, not wrapped");
        std::vector<uint8_t> buf2(4 * 4, 0);
        f.blit("X", buf2, 4, 4, -1, -1);       // wholly off the top-left but one pixel
        lit = 0;
        for (uint8_t b : buf2) lit += b;
        check(lit == 1, "negative positions clip too");
    }

    // Malformed input raises rather than allocating something absurd.
    {
        bool threw = false;
        try { ta::fnt::parse({1, 2, 3}, "<tiny>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a file shorter than the header is rejected");
        threw = false;
        std::vector<uint8_t> zeroH(4 + 512, 0);
        try { ta::fnt::parse(zeroH, "<zero>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a zero height is rejected");
    }

    std::printf(g_fail ? "fnt_test: %d FAILURE(S)\n" : "fnt_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
