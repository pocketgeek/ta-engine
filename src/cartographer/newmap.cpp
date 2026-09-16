#include "cartographer/newmap.h"

#include "cartographer/sections.h"
#include "hpi/hpi.h"
#include "terrain/terrain.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cart {

ta::gaf::Palette loadWorldPalette(const ta::hpi::Vfs& vfs, const std::string& world) {
    ta::gaf::Palette pal{};
    // Default: grayscale ramp (so a missing PCX still yields a usable index map).
    for (int i = 0; i < 256; ++i) {
        pal.rgba[i][0] = pal.rgba[i][1] = pal.rgba[i][2] = uint8_t(i);
        pal.rgba[i][3] = 255;
    }
    try {
        auto d = vfs.read("palettes/" + world + ".pcx");
        // A palette-only PCX ends with a 0x0C marker byte followed by 768 RGB
        // bytes (the 256-colour VGA palette).
        if (d.size() >= 769 && d[d.size() - 769] == 0x0C) {
            const uint8_t* p = d.data() + d.size() - 768;
            for (int i = 0; i < 256; ++i) {
                pal.rgba[i][0] = p[i * 3 + 0];
                pal.rgba[i][1] = p[i * 3 + 1];
                pal.rgba[i][2] = p[i * 3 + 2];
                pal.rgba[i][3] = 255;
            }
        }
    } catch (const std::exception&) { /* keep the grayscale ramp */ }
    return pal;
}

namespace {

uint8_t nearestIndex(const ta::gaf::Palette& pal, int r, int g, int b) {
    int best = 0, bestD = 1 << 30;
    for (int i = 0; i < 256; ++i) {
        int dr = r - pal.rgba[i][0], dg = g - pal.rgba[i][1], db = b - pal.rgba[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return uint8_t(best);
}

// Average each 32px block to one RGB, giving a blocksX x blocksY colour grid.
std::vector<uint8_t> blockColourGrid(const ta::tnt::Map& map,
                                     ta::terrain::Compositor& comp) {
    std::vector<uint8_t> grid(size_t(map.blocksX) * map.blocksY * 3, 0);
    std::vector<uint8_t> block(32 * 32 * 4);
    for (int by = 0; by < map.blocksY; ++by)
        for (int bx = 0; bx < map.blocksX; ++bx) {
            std::fill(block.begin(), block.end(), 0);
            comp.renderBlock(map, bx, by, block, 32, 0, 0);
            long r = 0, g = 0, b = 0;
            for (int i = 0; i < 32 * 32; ++i) {
                r += block[i * 4]; g += block[i * 4 + 1]; b += block[i * 4 + 2];
            }
            size_t gi = (size_t(by) * map.blocksX + bx) * 3;
            grid[gi] = uint8_t(r / (32 * 32));
            grid[gi + 1] = uint8_t(g / (32 * 32));
            grid[gi + 2] = uint8_t(b / (32 * 32));
        }
    return grid;
}

// Resample the block-colour grid to (tw x th) and index against the palette.
std::vector<uint8_t> indexedMinimap(const std::vector<uint8_t>& grid, int gw, int gh,
                                    int tw, int th, const ta::gaf::Palette& pal) {
    std::vector<uint8_t> out(size_t(tw) * th, 0);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            int sx = gw > 0 ? std::min(gw - 1, x * gw / tw) : 0;
            int sy = gh > 0 ? std::min(gh - 1, y * gh / th) : 0;
            const uint8_t* c = &grid[(size_t(sy) * gw + sx) * 3];
            out[size_t(y) * tw + x] = nearestIndex(pal, c[0], c[1], c[2]);
        }
    return out;
}

} // namespace

void generateMinimaps(ta::tnt::Map& map, ta::terrain::Compositor& comp,
                      const ta::gaf::Palette& pal) {
    if (map.blocksX <= 0 || map.blocksY <= 0) return;
    std::vector<uint8_t> grid = blockColourGrid(map, comp);

    // Small minimap: retail's fixed 126x126.
    map.minimapW = 126; map.minimapH = 126;
    map.minimap = indexedMinimap(grid, map.blocksX, map.blocksY, 126, 126, pal);

    // Large overview: fit the map aspect into a ~430px box (retail's exact dims
    // are a computed thumbnail scale; this matches its ballpark and always loads).
    int longSide = std::max(map.blocksX, map.blocksY);
    float scale = longSide > 0 ? 430.0f / longSide : 1.0f;
    int ow = std::max(1, int(map.blocksX * scale));
    int oh = std::max(1, int(map.blocksY * scale));
    map.overviewW = ow; map.overviewH = oh;
    map.overview = indexedMinimap(grid, map.blocksX, map.blocksY, ow, oh, pal);
}

void resizeMap(ta::tnt::Map& map, ta::terrain::Compositor& comp,
               const ta::gaf::Palette& pal, int wUnits, int hUnits) {
    int nw = std::max(1, wUnits) * 32, nh = std::max(1, hUnits) * 32;
    int nbx = nw / 2, nby = nh / 2;
    if (nw == map.width && nh == map.height) return;
    size_t ncells = size_t(nw) * nh, nblocks = size_t(nbx) * nby;

    uint8_t fillH = uint8_t(map.seaLevel + 22);   // flat land for new area
    uint32_t fk = map.tileKeys.empty() ? 0 : map.tileKeys[0];
    uint8_t fc = map.tileCols.empty() ? 0 : map.tileCols[0];
    uint8_t fr = map.tileRows.empty() ? 0 : map.tileRows[0];

    std::vector<uint8_t> h(ncells, fillH);
    std::vector<uint16_t> ft(ncells, 0xFFFF);
    std::vector<uint32_t> tk(nblocks, fk);
    std::vector<uint8_t> tc(nblocks, fc), tr(nblocks, fr);

    int cw = std::min(map.width, nw), ch = std::min(map.height, nh);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            h[size_t(y) * nw + x] = map.heights[size_t(y) * map.width + x];
            ft[size_t(y) * nw + x] = map.features[size_t(y) * map.width + x];
        }
    int cbx = std::min(map.blocksX, nbx), cby = std::min(map.blocksY, nby);
    for (int y = 0; y < cby; ++y)
        for (int x = 0; x < cbx; ++x) {
            size_t s = size_t(y) * map.blocksX + x, d = size_t(y) * nbx + x;
            tk[d] = map.tileKeys[s]; tc[d] = map.tileCols[s]; tr[d] = map.tileRows[s];
        }

    map.width = nw; map.height = nh; map.blocksX = nbx; map.blocksY = nby;
    map.heights = std::move(h); map.features = std::move(ft);
    map.tileKeys = std::move(tk); map.tileCols = std::move(tc); map.tileRows = std::move(tr);
    generateMinimaps(map, comp, pal);
}

ta::tnt::Map newBlankMap(const ta::hpi::Vfs& vfs, SectionLibrary& sections,
                          ta::terrain::Compositor& comp, const std::string& world,
                          int wUnits, int hUnits) {
    // Pick a flat ground section for the fill (prefer a "flat"/"ground" category).
    const SectionRef* fill = nullptr;
    for (const auto& s : sections.list()) {
        std::string c = s.category;
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        if (c.find("flat") != std::string::npos || c.find("ground") != std::string::npos) {
            fill = &s; break;
        }
    }
    if (!fill && !sections.list().empty()) fill = &sections.list().front();
    if (!fill) return {};
    const ta::tnt::Map* sec = sections.load(vfs, fill->path);
    if (!sec) return {};

    ta::tnt::Map m;
    m.width = std::max(1, wUnits) * 32;    // 1 Unit = 32 cells
    m.height = std::max(1, hUnits) * 32;
    m.blocksX = m.width / 2;
    m.blocksY = m.height / 2;
    m.seaLevel = (world == "aramon") ? 40 : 58;   // sidedata waterheight per side
    size_t cells = size_t(m.width) * m.height;
    size_t blocks = size_t(m.blocksX) * m.blocksY;
    m.heights.assign(cells, uint8_t(m.seaLevel + 22));   // flat land above water
    m.features.assign(cells, 0xFFFF);
    m.tileKeys.assign(blocks, 0);
    m.tileCols.assign(blocks, 0);
    m.tileRows.assign(blocks, 0);

    // Tile the fill section across the whole map (it carries flat heights + art).
    for (int by = 0; by < m.blocksY; by += sec->blocksY)
        for (int bx = 0; bx < m.blocksX; bx += sec->blocksX)
            stampSection(m, *sec, bx, by);

    generateMinimaps(m, comp, loadWorldPalette(vfs, world));
    return m;
}

} // namespace cart
