#include "cartographer/newmap.h"

#include "cartographer/sections.h"
#include "hpi/hpi.h"
#include "terrain/terrain.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <stdexcept>
#include <tuple>

namespace cart {

void resizeMap(ta::tnt::Map& map, ta::terrain::Compositor& comp,
               const ta::gaf::Palette& pal, int wCells, int hCells) {
    // TNT dims are 16px cells and the tile plane is one entry per 32px block, so a
    // size is a whole number of blocks (an even cell count).
    const int nw = std::max(2, wCells & ~1), nh = std::max(2, hCells & ~1);
    const int nbx = nw / 2, nby = nh / 2;
    if (nw == map.width && nh == map.height) return;
    const size_t ncells = size_t(nw) * nh, nblocks = size_t(nbx) * nby;
    const bool ta = ownTiles(map);

    // New area: the top-left block's art at its height (TA), else flat land.
    const uint8_t fillH = ta && !map.heights.empty() ? map.heights[0] : uint8_t(map.seaLevel + 22);
    const uint32_t fk = map.tileKeys.empty() ? 0 : map.tileKeys[0];
    const uint8_t fc = map.tileCols.empty() ? 0 : map.tileCols[0];
    const uint8_t fr = map.tileRows.empty() ? 0 : map.tileRows[0];
    const uint16_t fi = map.tileIndex.empty() ? 0 : map.tileIndex[0];

    std::vector<uint8_t> h(ncells, fillH), extra(ncells, 0);
    std::vector<uint16_t> ft(ncells, 0xFFFF);
    std::vector<uint32_t> tk(nblocks, fk);
    std::vector<uint8_t> tc(nblocks, fc), tr(nblocks, fr);
    std::vector<uint16_t> ti(ta ? nblocks : 0, fi);

    const int cw = std::min(map.width, nw), ch = std::min(map.height, nh);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t s = size_t(y) * map.width + x, d = size_t(y) * nw + x;
            h[d] = map.heights[s];
            ft[d] = map.features[s];
            if (s < map.cellExtra.size()) extra[d] = map.cellExtra[s];
        }
    const int cbx = std::min(map.blocksX, nbx), cby = std::min(map.blocksY, nby);
    for (int y = 0; y < cby; ++y)
        for (int x = 0; x < cbx; ++x) {
            const size_t s = size_t(y) * map.blocksX + x, d = size_t(y) * nbx + x;
            tk[d] = map.tileKeys[s]; tc[d] = map.tileCols[s]; tr[d] = map.tileRows[s];
            if (ta) ti[d] = map.tileIndex[s];
        }

    map.width = nw; map.height = nh; map.blocksX = nbx; map.blocksY = nby;
    map.heights = std::move(h); map.features = std::move(ft); map.cellExtra = std::move(extra);
    map.tileKeys = std::move(tk); map.tileCols = std::move(tc); map.tileRows = std::move(tr);
    if (ta) {
        map.tileIndex = std::move(ti);
        compactTiles(map);   // a crop leaves the cut-off art unused
    }
    generateMinimaps(map, comp, pal);
}

namespace {
// How well a section fills a blank map: a flat/ground category first, then the
// smallest height range (a level piece), then land over water, then no objects.
struct FillScore {
    int category = 1, range = 255, water = 1, objects = 1;
    auto key() const { return std::tuple(category, objects, range, water); }
};
FillScore fillScore(const SectionRef& ref, const ta::tnt::Map& m) {
    FillScore s;
    std::string c = ref.category;
    std::transform(c.begin(), c.end(), c.begin(), ::tolower);
    s.category = c.find("flat") != std::string::npos || c.find("ground") != std::string::npos ? 0 : 1;
    if (!m.heights.empty()) {
        const auto [lo, hi] = std::minmax_element(m.heights.begin(), m.heights.end());
        s.range = *hi - *lo;
        s.water = *lo < 16 ? 1 : 0;   // TA sections' land sits well above sea level 0
    }
    s.objects = std::all_of(m.features.begin(), m.features.end(), [](uint16_t v) { return v == 0xFFFF; }) ? 0 : 1;
    return s;
}
}  // namespace

ta::tnt::Map newBlankMap(const ta::hpi::Vfs& vfs, SectionLibrary& sections,
                          ta::terrain::Compositor& comp, const std::string& world,
                          int wUnits, int hUnits) {
    // The fill: a flat ("flat"/"ground" category) section with nothing in its
    // feature plane (the first Taros and Veruna flats were cobbles, whose road
    // markers would pave the whole map), else the levellest land section: TA's
    // Archipelago has no flat category. Ties keep the library's order.
    const SectionRef* fill = nullptr;
    const ta::tnt::Map* sec = nullptr;
    FillScore best;
    // Flat categories first (a few pieces); every section only when there is none.
    for (const bool flatOnly : {true, false}) {
        for (const auto& s : sections.list()) {
            std::string c = s.category;
            std::transform(c.begin(), c.end(), c.begin(), ::tolower);
            if (flatOnly && c.find("flat") == std::string::npos && c.find("ground") == std::string::npos) continue;
            const ta::tnt::Map* m = sections.load(vfs, s.path);
            if (!m || m->blocksX <= 0 || m->blocksY <= 0) continue;
            const auto score = fillScore(s, *m);
            if (!fill || score.key() < best.key()) { fill = &s; sec = m; best = score; }
            if (best.key() == std::tuple(0, 0, 0, 0)) break;
        }
        if (fill) break;
    }
    if (!fill || !sec) return {};

    ta::tnt::Map m;
    m.width = std::max(1, wUnits) * 32;    // 1 Unit = 32 cells
    m.height = std::max(1, hUnits) * 32;
    m.blocksX = m.width / 2;
    m.blocksY = m.height / 2;
    const bool ta = sec->tileCount > 0;
    // TA: sea level 0, as the random maps (mapgen_ta.cpp); the fill's own heights
    // decide land and water. Kingdoms: sidedata waterheight per side.
    m.seaLevel = ta ? 0 : world == "aramon" ? 40 : world == "volcano" ? 56 : 58;
    const size_t cells = size_t(m.width) * m.height;
    const size_t blocks = size_t(m.blocksX) * m.blocksY;
    m.heights.assign(cells, uint8_t(m.seaLevel + 22));   // flat land above water
    m.features.assign(cells, 0xFFFF);
    m.cellExtra.assign(cells, 0);
    m.tileKeys.assign(blocks, 0);
    m.tileCols.assign(blocks, 0);
    m.tileRows.assign(blocks, 0);
    if (ta) ensureTilePlane(m);

    // Tile the fill section across the whole map (it carries flat heights + art).
    for (int by = 0; by < m.blocksY; by += sec->blocksY)
        for (int bx = 0; bx < m.blocksX; bx += sec->blocksX)
            stampSection(m, *sec, bx, by);

    generateMinimaps(m, comp, loadWorldPalette(vfs, world));
    return m;
}

} // namespace cart
