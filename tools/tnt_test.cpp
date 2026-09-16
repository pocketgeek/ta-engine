// tnt_test -- the TA TNT map reader/writer (version 0x2000).
//
// Retail maps cannot live on a CI runner, so this builds synthetic TNTs and
// round-trips them. The real validation is `tnttool roundtrip`, which reproduces
// all 96 shipped maps byte-for-byte; what is pinned here is the handful of rules
// that were expensive to work out and would be silent to break:
//
//   * MapAttr is { u8 height; u16 feature; u8 unused } -- the u16 UNALIGNED at
//     byte 1. Reading it as an aligned field yields plausible-looking garbage
//     rather than an error, which is the worst kind of wrong.
//   * The tile plane is padded to a 16-BYTE boundary and nothing else is. Every
//     shipped map agrees; 4-byte alignment matches only 58 of 96.
//   * 0xFFFE means "covered by a multi-cell feature", stored in the file. Losing
//     it would make a 3x3 metal patch read as nine separate patches.
//   * Tile indices are per-map. `tile()` must refuse one the library lacks
//     instead of walking off the end of tileGfx.

#include "tnt/tnt.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}

// A small map with a known shape: 4x4 cells (2x2 tiles), three tiles, one 2x2
// feature anchored at cell (1,1) with its other three cells marked covered.
static ta::tnt::Map sample() {
    ta::tnt::Map m;
    m.width = 4; m.height = 4;
    m.blocksX = 2; m.blocksY = 2;
    m.seaLevel = 50;
    m.numTiles = 3;
    m.tileGfx.resize(size_t(m.numTiles) * ta::tnt::kTileBytes);
    for (int t = 0; t < m.numTiles; ++t)
        for (int i = 0; i < ta::tnt::kTileBytes; ++i)
            m.tileGfx[size_t(t) * ta::tnt::kTileBytes + i] = uint8_t((t * 37 + i) & 0xff);
    m.tiles = {0, 1, 2, 1};
    m.heights.assign(16, 0);
    for (int i = 0; i < 16; ++i) m.heights[i] = uint8_t(40 + i);
    m.features.assign(16, ta::tnt::kNoFeature);
    m.features[1 * 4 + 1] = 0;                       // anchor
    m.features[1 * 4 + 2] = ta::tnt::kFeatureCovered;
    m.features[2 * 4 + 1] = ta::tnt::kFeatureCovered;
    m.features[2 * 4 + 2] = ta::tnt::kFeatureCovered;
    m.featureNames = {"ArchMetal1"};
    m.minimapW = 4; m.minimapH = 4;
    m.minimap.assign(16, 7);
    return m;
}

int main() {
    std::printf("tnt_test\n");

    const ta::tnt::Map src = sample();
    const std::vector<uint8_t> bytes = src.save();
    const ta::tnt::Map r = ta::tnt::Map::load(bytes, "<synthetic>");

    check(r.width == src.width && r.height == src.height && r.seaLevel == src.seaLevel,
          "dims and sea level survive a round trip");
    check(r.heights == src.heights, "heights survive");
    check(r.features == src.features, "features survive");
    check(r.tiles == src.tiles, "tile indices survive");
    check(r.numTiles == src.numTiles && r.tileGfx == src.tileGfx, "the tile library survives");
    check(r.featureNames == src.featureNames, "feature names survive");
    check(r.minimapW == src.minimapW && r.minimap == src.minimap, "the minimap survives");

    // The sentinel must come back as itself, not collapse into "no feature".
    check(r.features[1 * 4 + 2] == ta::tnt::kFeatureCovered &&
          r.features[1 * 4 + 1] == 0,
          "0xFFFE (covered by a multi-cell feature) is preserved distinctly");

    // MapAttr layout, asserted against the bytes rather than via the reader --
    // a reader/writer pair that agreed on the WRONG offset would round-trip fine.
    {
        auto word = [&](int i) {
            size_t o = size_t(i) * 4;
            return uint32_t(bytes[o] | (bytes[o+1] << 8) | (bytes[o+2] << 16) |
                            (uint32_t(bytes[o+3]) << 24));
        };
        check(word(0) == 0x2000, "header declares version 0x2000");
        size_t attr = word(4);
        // Cell (1,1) = index 5: height 45, feature 0.
        check(bytes[attr + 5 * 4 + 0] == 45, "MapAttr byte 0 is the height");
        check(bytes[attr + 5 * 4 + 1] == 0 && bytes[attr + 5 * 4 + 2] == 0,
              "MapAttr bytes 1-2 are the feature u16, unaligned");
        check(bytes[attr + 5 * 4 + 3] == 0, "MapAttr byte 3 is the unused one");
        // Cell (2,1) = index 6 holds 0xFFFE across bytes 1-2.
        check(bytes[attr + 6 * 4 + 1] == 0xFE && bytes[attr + 6 * 4 + 2] == 0xFF,
              "the covered sentinel is written little-endian at byte 1");

        // Tile plane: 2x2 tiles = 8 bytes from offset 64, padded to 16 bytes.
        check(word(3) == 64, "the tile plane follows the 64-byte header");
        check(attr == 80, "and the MapAttr plane starts at the next 16-byte boundary");
    }

    // Out-of-range tile lookups must be refused, not clamped or wrapped: a map
    // referencing a tile its library lacks is a real thing (the editor can make
    // one), and reading past tileGfx would be a heap overread per frame.
    check(src.tile(0) != nullptr, "tile(0) resolves");
    check(src.tile(2) != nullptr, "tile(numTiles-1) resolves");
    check(src.tile(3) == nullptr, "tile(numTiles) is refused");
    check(src.tile(-1) == nullptr, "a negative tile index is refused");

    // A truncated or foreign file must raise rather than read wild offsets.
    {
        bool threw = false;
        std::vector<uint8_t> tak = bytes;
        tak[0] = 0x00; tak[1] = 0x40;         // version 0x4000 = a Kingdoms map
        try { ta::tnt::Map::load(tak, "<tak>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a Kingdoms 0x4000 map is rejected, not misread");

        threw = false;
        std::vector<uint8_t> tiny(bytes.begin(), bytes.begin() + 40);
        try { ta::tnt::Map::load(tiny, "<tiny>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a file shorter than the header is rejected");

        threw = false;
        std::vector<uint8_t> cut = bytes;
        cut.resize(bytes.size() / 2);          // planes now overrun the buffer
        try { ta::tnt::Map::load(cut, "<cut>"); } catch (const std::exception&) { threw = true; }
        check(threw, "a truncated file is rejected rather than read past the end");
    }

    std::printf(g_fail ? "tnt_test: %d FAILURE(S)\n" : "tnt_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
