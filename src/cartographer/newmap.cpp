#include "cartographer/newmap.h"

#include "cartographer/sections.h"
#include "hpi/hpi.h"
#include "terrain/terrain.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <stdexcept>

namespace cart {

void resizeMap(tak::tnt::Map& map, tak::terrain::Compositor& comp,
               const tak::gaf::Palette& pal, int wUnits, int hUnits) {
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

tak::tnt::Map newBlankMap(const tak::hpi::Vfs& vfs, SectionLibrary& sections,
                          tak::terrain::Compositor& comp, const std::string& world,
                          int wUnits, int hUnits) {
    // Pick a flat ground section for the fill (prefer a "flat"/"ground" category)
    // with nothing in its feature plane: the first Taros and Veruna flats are
    // cobbles, whose road markers would pave the whole map (and refuse every
    // feature move or paste, which protect terrain markers).
    const SectionRef* fill = nullptr;
    const SectionRef* plain = nullptr;
    for (const auto& s : sections.list()) {
        std::string c = s.category;
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        if (c.find("flat") == std::string::npos && c.find("ground") == std::string::npos) continue;
        if (!fill) fill = &s;
        const tak::tnt::Map* m = sections.load(vfs, s.path);
        if (m && std::all_of(m->features.begin(), m->features.end(), [](uint16_t v) { return v == 0xFFFF; })) { plain = &s; break; }
    }
    if (plain) fill = plain;
    if (!fill && !sections.list().empty()) fill = &sections.list().front();
    if (!fill) return {};
    const tak::tnt::Map* sec = sections.load(vfs, fill->path);
    if (!sec) return {};

    tak::tnt::Map m;
    m.width = std::max(1, wUnits) * 32;    // 1 Unit = 32 cells
    m.height = std::max(1, hUnits) * 32;
    m.blocksX = m.width / 2;
    m.blocksY = m.height / 2;
    // sidedata waterheight per side; the volcano's is takx20's (its lava lies at 0).
    m.seaLevel = world == "aramon" ? 40 : world == "volcano" ? 56 : 58;
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
