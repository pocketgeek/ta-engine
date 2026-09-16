// sct_test -- worlds.hpi's ".sct" section prefabs (the editor's stamp palette).
//
// Data-independent, so CI runs it: the files are hand-assembled from the format
// definition rather than round-tripped through a writer of ours.
//
// The rules worth pinning are the ones that were expensive to establish and would
// be silent to break:
//
//   * THE TWO VERSIONS PUT THEIR PLANES IN A DIFFERENT ORDER. In version 2 the
//     tile graphics sit immediately after the header and the cell planes follow
//     them; in version 3 the planes come first and the graphics move to the back.
//     Both headers are pointer-led, so reading one with the other's field meaning
//     yields plausible in-range offsets rather than an error.
//   * The attribute plane is FLAT and row-major over the whole section -- not
//     grouped per tile. The per-tile grouping is exactly the same size in bytes,
//     so nothing about the file's shape distinguishes them; what does is that the
//     flat reading yields terrain with a little under half the height variation
//     of either per-tile ordering, across every shipped section.
//   * The record is 4 bytes in version 3 (byte-identical to the TNT MapAttr, the
//     feature u16 UNALIGNED at byte 1) and 8 in version 2.
//   * width/height are in 32px TILES, so the cell dimensions are twice each.
//   * The thumbnail is a fixed 128x128 regardless of the section's own size or
//     aspect; a non-square section is drawn into it letterboxed.

#include "sct/sct.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void eqi(int got, int want, const std::string& what) {
    bool ok = got == want;
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) { std::printf("      got %d, want %d\n", got, want); ++g_fail; }
}

static void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = uint8_t(v); d[o + 1] = uint8_t(v >> 8);
    d[o + 2] = uint8_t(v >> 16); d[o + 3] = uint8_t(v >> 24);
}

// Build a section: `w` x `h` tiles, `nTiles` distinct tiles in the library.
// Heights are cell index & 0xFF so a misordered plane is visible immediately.
static std::vector<uint8_t> build(int version, int w, int h, int nTiles) {
    const int hdrWords = version == 3 ? 8 : 7;
    const size_t hdr = size_t(hdrWords) * 4;
    const size_t nTilesXY = size_t(w) * size_t(h);
    const size_t nCells = size_t(w * 2) * size_t(h * 2);
    const size_t stride = version == 3 ? 4 : 8;
    const size_t gfxBytes = size_t(nTiles) * 1024;
    const size_t planeBytes = nTilesXY * 2 + nCells * stride;

    // Version 2: header, graphics, planes, thumbnail.
    // Version 3: header, planes, graphics, thumbnail.
    size_t pGfx, pIndex;
    if (version == 2) { pGfx = hdr;              pIndex = hdr + gfxBytes; }
    else              { pIndex = hdr;            pGfx   = hdr + planeBytes; }
    const size_t pThumb = (version == 2 ? pIndex + planeBytes : pGfx + gfxBytes);

    std::vector<uint8_t> d(pThumb + 128 * 128, 0);
    put32(d, 0,  uint32_t(version));
    put32(d, 4,  uint32_t(pThumb));
    put32(d, 8,  uint32_t(nTiles));
    put32(d, 12, uint32_t(pGfx));
    put32(d, 16, uint32_t(w));
    put32(d, 20, uint32_t(h));
    put32(d, 24, uint32_t(pIndex));
    if (version == 3) put32(d, 28, 65536);      // the flags word

    for (size_t i = 0; i < gfxBytes; ++i) d[pGfx + i] = uint8_t((i * 7 + i / 1024) & 0xff);
    for (size_t i = 0; i < nTilesXY; ++i) {
        uint16_t ti = uint16_t(i % size_t(nTiles));
        d[pIndex + i * 2] = uint8_t(ti);
        d[pIndex + i * 2 + 1] = uint8_t(ti >> 8);
    }
    const size_t attr = pIndex + nTilesXY * 2;
    for (size_t c = 0; c < nCells; ++c) {
        const size_t o = attr + c * stride;
        d[o] = uint8_t(c & 0xff);               // height
        if (version == 3) { d[o + 1] = 0xFF; d[o + 2] = 0xFF; }   // feature = none
        else { d[o + 1] = 0x01; d[o + 2] = 0xFF; }                // v2's invariant tail
    }
    for (size_t i = 0; i < 128 * 128; ++i) d[pThumb + i] = uint8_t(i & 0x7f);
    return d;
}

static void exercise(int version) {
    const int w = 5, h = 3, nTiles = 4;
    auto bytes = build(version, w, h, nTiles);
    auto m = ta::sct::load(bytes, "<synthetic>");

    const std::string v = "v" + std::to_string(version) + ": ";
    eqi(m.blocksX, w, v + "width is read in 32px tiles");
    eqi(m.blocksY, h, v + "height too");
    eqi(m.width, w * 2, v + "and the cell width is twice it");
    eqi(m.height, h * 2, v + "as is the cell height");
    eqi(m.numTiles, nTiles, v + "the tile count reads");
    eqi(int(m.tileGfx.size()), nTiles * 1024, v + "and its graphics are all present");
    eqi(int(m.tiles.size()), w * h, v + "one tile index per tile");

    // Every index must resolve -- if the graphics pointer were read as the plane
    // pointer (the two swap between versions) this is what would break.
    bool allResolve = true;
    for (uint16_t t : m.tiles) if (!m.tile(int(t))) allResolve = false;
    check(allResolve, v + "every tile index resolves inside the library");
    // ...and the graphics really are the graphics, not a plane misread as one.
    check(m.tile(0) && m.tile(0)[0] == 0 && m.tile(0)[1] == 7,
          v + "tile 0's bytes are the graphics, read from the right offset");

    // The flat row-major plane: height of cell (x,y) is (y*W + x) & 0xFF.
    eqi(int(m.heights.size()), w * 2 * h * 2, v + "one height per 16px cell");
    bool flat = true;
    for (int y = 0; y < h * 2; ++y)
        for (int x = 0; x < w * 2; ++x)
            if (m.heights[size_t(y) * size_t(w * 2) + size_t(x)] !=
                uint8_t((y * w * 2 + x) & 0xff))
                flat = false;
    check(flat, v + "the attribute plane is FLAT row-major, not grouped per tile");

    // Features: version 3 carries the TNT sentinel; version 2 has no readable
    // field, so the plane must come back as "no feature" rather than as garbage.
    bool noFeat = true;
    for (uint16_t f : m.features) if (f != ta::tnt::kNoFeature) noFeat = false;
    check(noFeat, v + "the feature plane reads as no-feature throughout");

    eqi(m.minimapW, 128, v + "the thumbnail is 128 wide");
    eqi(m.minimapH, 128, v + "and 128 tall, whatever the section's own size");
    eqi(int(m.minimap.size()), 128 * 128, v + "and is fully read");
    eqi(m.seaLevel, 0, v + "a section carries no sea level");
}

int main() {
    std::printf("sct_test\n");

    exercise(2);
    exercise(3);

    // A non-square section keeps its 128x128 thumbnail: the aspect lives in how
    // the image is drawn (letterboxed), not in the block's dimensions.
    {
        auto m = ta::sct::load(build(3, 2, 8, 3), "<tall>");
        eqi(m.blocksX, 2, "a tall section's width");
        eqi(m.blocksY, 8, "and height");
        eqi(m.minimapW, 128, "still carry a 128x128 thumbnail");
    }

    // Reading one version with the other's field order must not silently work.
    // Version 3's header says the planes start at 28; in version 2 that offset is
    // the graphics. Flipping the version byte alone therefore mis-seats every
    // plane -- the loader must notice, via the bounds the pointers imply.
    {
        auto v3 = build(3, 5, 3, 4);
        v3[0] = 2;                     // claim version 2, keep v3's layout
        bool threwOrDiffered = false;
        try {
            auto m = ta::sct::load(v3, "<mislabelled>");
            // If it parses at all, it must not agree with the correct reading.
            auto good = ta::sct::load(build(3, 5, 3, 4), "<good>");
            threwOrDiffered = m.heights != good.heights || m.tileGfx != good.tileGfx;
        } catch (const std::exception&) { threwOrDiffered = true; }
        check(threwOrDiffered,
              "a version-2 read of a version-3 file does not quietly agree with it");
    }

    // Malformed input raises rather than reading wild offsets.
    {
        bool threw = false;
        try { ta::sct::load({1, 2, 3}, "<tiny>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a file shorter than a header is rejected");

        threw = false;
        auto bad = build(2, 5, 3, 4);
        put32(bad, 0, 7);              // an unknown version
        try { ta::sct::load(bad, "<v7>"); } catch (const std::exception&) { threw = true; }
        check(threw, "an unknown version is rejected, not guessed at");

        // Truncated INTO the planes: the geometry a stamp needs is gone, so this
        // must raise rather than read past the end.
        threw = false;
        auto cut = build(2, 5, 3, 4);
        cut.resize(100);               // inside the tile graphics
        try { ta::sct::load(cut, "<cut>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a file truncated inside its planes is rejected");

        // Truncated to JUST the thumbnail: everything a stamp needs is present,
        // and the thumbnail is decoration. That loads, with no minimap -- stated
        // here so the leniency is a decision on record rather than an accident.
        auto noThumb = build(2, 5, 3, 4);
        noThumb.resize(noThumb.size() - 128 * 128);
        bool loaded = false;
        int mmW = -1;
        try {
            auto m = ta::sct::load(noThumb, "<nothumb>");
            loaded = m.heights.size() == 60 && m.tiles.size() == 15;
            mmW = m.minimapW;
        } catch (const std::exception&) {}
        check(loaded, "a section missing only its thumbnail still loads");
        eqi(mmW, 0, "and reports no minimap rather than a partial one");

        threw = false;
        auto zero = build(2, 5, 3, 4);
        put32(zero, 16, 0);            // zero width
        try { ta::sct::load(zero, "<zero>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a zero-sized section is rejected");
    }

    check(ta::sct::isSectionPath("sections/Archipelago/coast/CoastH.SCT"),
          "the extension test is case-insensitive");
    check(!ta::sct::isSectionPath("maps/Coast To Coast.tnt"),
          "and does not claim a TNT");

    std::printf(g_fail ? "sct_test: %d FAILURE(S)\n" : "sct_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
