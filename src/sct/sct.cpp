#include "sct/sct.h"

#include "util/strcase.h"

#include <stdexcept>

namespace ta::sct {
namespace {

uint32_t u32At(const std::vector<uint8_t>& d, size_t o) {
    return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) |
           (uint32_t(d[o + 3]) << 24);
}

[[noreturn]] void fail(const std::string& origin, const std::string& why) {
    throw std::runtime_error("sct " + origin + ": " + why);
}

// The palette thumbnail is a fixed 128x128 8-bit image in every shipped section,
// regardless of the section's own size.
constexpr int kThumbW = 128, kThumbH = 128;
constexpr size_t kThumbBytes = size_t(kThumbW) * kThumbH;

} // namespace

bool isSectionPath(const std::string& path) { return ta::iendsWith(path, ".sct"); }

tnt::Map load(const std::vector<uint8_t>& d, const std::string& origin) {
    if (d.size() < 28) fail(origin, "shorter than a header");
    const uint32_t version = u32At(d, 0);
    if (version != 2 && version != 3)
        fail(origin, "unsupported version " + std::to_string(version));

    const uint32_t pThumb = u32At(d, 4);
    const uint32_t numTiles = u32At(d, 8);
    const uint32_t pGfx = u32At(d, 12);
    const uint32_t wTiles = u32At(d, 16);
    const uint32_t hTiles = u32At(d, 20);
    const uint32_t pIndex = u32At(d, 24);
    if (version == 3 && d.size() < 32) fail(origin, "shorter than a version 3 header");

    if (wTiles == 0 || hTiles == 0) fail(origin, "zero-sized section");
    // Bound the dimensions before any of them is multiplied out.
    if (wTiles > 4096 || hTiles > 4096)
        fail(origin, "implausible size " + std::to_string(wTiles) + "x" + std::to_string(hTiles));

    tnt::Map m;
    m.blocksX = int(wTiles);
    m.blocksY = int(hTiles);
    m.width = int(wTiles) * 2;      // 16px cells
    m.height = int(hTiles) * 2;
    m.seaLevel = 0;                 // not carried by the container; see sct.h
    m.numTiles = int(numTiles);

    const size_t nTilesXY = size_t(wTiles) * hTiles;
    const size_t nCells = size_t(m.width) * size_t(m.height);
    // 4 bytes per cell in version 3 (the TNT MapAttr), 8 in version 2.
    const size_t cellStride = version == 3 ? 4 : 8;

    // Tile graphics.
    const size_t gfxBytes = size_t(numTiles) * tnt::kTileBytes;
    if (size_t(pGfx) > d.size() || gfxBytes > d.size() - pGfx)
        fail(origin, "tile graphics run past the end of the file");
    m.tileGfx.assign(d.begin() + pGfx, d.begin() + pGfx + gfxBytes);

    // Tile index plane, then the flat per-cell attribute plane after it.
    const size_t idxBytes = nTilesXY * 2;
    const size_t attrBytes = nCells * cellStride;
    if (size_t(pIndex) > d.size() || idxBytes > d.size() - pIndex ||
        attrBytes > d.size() - pIndex - idxBytes)
        fail(origin, "the cell planes run past the end of the file");

    m.tiles.resize(nTilesXY);
    for (size_t i = 0; i < nTilesXY; ++i)
        m.tiles[i] = uint16_t(d[pIndex + i * 2] | (uint16_t(d[pIndex + i * 2 + 1]) << 8));

    const size_t attr = size_t(pIndex) + idxBytes;
    m.heights.resize(nCells);
    m.features.assign(nCells, tnt::kNoFeature);
    for (size_t i = 0; i < nCells; ++i) {
        const size_t o = attr + i * cellStride;
        m.heights[i] = d[o];
        // Version 3's record is the TNT MapAttr, so its feature is readable and
        // carried across. Version 2's remaining bytes are invariant across every
        // shipped section and are deliberately not guessed at -- the plane stays
        // "no feature", which is what all 607 of them describe anyway.
        if (version == 3)
            m.features[i] = uint16_t(d[o + 1] | (uint16_t(d[o + 2]) << 8));
    }

    // Thumbnail, into the map's minimap slot -- the palette draws it directly.
    // Its absence is not fatal: the geometry above is what a stamp needs.
    if (size_t(pThumb) <= d.size() && kThumbBytes <= d.size() - pThumb) {
        m.minimapW = kThumbW;
        m.minimapH = kThumbH;
        m.minimap.assign(d.begin() + pThumb, d.begin() + pThumb + kThumbBytes);
    }
    return m;
}

} // namespace ta::sct
