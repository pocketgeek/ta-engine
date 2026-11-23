#include "terrain/minimap.h"
#include "hpi/hpi.h"
#include "terrain/terrain.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <vector>

namespace ta::terrain {

ta::gaf::Palette loadWorldPalette(const ta::hpi::Vfs& vfs, const std::string& world) {
    ta::gaf::Palette pal{};
    // Default: grayscale ramp (so a missing PCX still yields a usable index map).
    for (int i = 0; i < 256; ++i) {
        pal.rgba[i][0] = pal.rgba[i][1] = pal.rgba[i][2] = uint8_t(i);
        pal.rgba[i][3] = 255;
    }
    // Total Annihilation: one game palette for every world (palettes/palette.pal).
    if (!vfs.has("palettes/" + world + ".pcx") && vfs.has("palettes/palette.pal")) {
        try { return ta::gaf::Palette::fromBytes(vfs.read("palettes/palette.pal"), "palette.pal"); }
        catch (const std::exception&) {}
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
                                     ta::terrain::Compositor& comp,std::shared_ptr<const std::atomic_bool> stop={},int width=0,int height=0) {
    const int gw=width?width:map.blocksX,gh=height?height:map.blocksY;
    std::vector<uint8_t> grid(size_t(gw) * gh * 3, 0);
    std::vector<uint8_t> block(32 * 32 * 4);
    std::map<uint64_t,std::array<uint8_t,3>> averages;
    for (int by = 0; by < gh; ++by) {
        if(stop && stop->load(std::memory_order_relaxed))return {};
        for (int bx = 0; bx < gw; ++bx) {
            const int sx=bx*map.blocksX/gw,sy=by*map.blocksY/gh;
            const size_t i=size_t(sy)*map.blocksX+sx;
            const uint64_t key=uint64_t(map.tileKeys[i]) | (uint64_t(map.tileCols[i])<<32) | (uint64_t(map.tileRows[i])<<40);
            auto found=averages.find(key);
            if(found==averages.end()) {
                std::fill(block.begin(),block.end(),0);
                comp.renderBlock(map,sx,sy,block,32,0,0);
                int r=0,g=0,b=0;
                for(int pixel=0;pixel<1024;++pixel) {r+=block[pixel*4];g+=block[pixel*4+1];b+=block[pixel*4+2];}
                found=averages.emplace(key,std::array<uint8_t,3>{uint8_t(r/1024),uint8_t(g/1024),uint8_t(b/1024)}).first;
            }
            std::copy(found->second.begin(),found->second.end(),grid.begin()+(size_t(by)*gw+bx)*3);
        }
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

MinimapPreview minimapPreview(const ta::tnt::Map& map,ta::terrain::Compositor& comp,
                              const ta::gaf::Palette& palette,std::shared_ptr<const std::atomic_bool> stop) {
    MinimapPreview result;
    if(map.blocksX<=0 || map.blocksY<=0)return result;
    const auto grid=blockColourGrid(map,comp,stop,result.width,result.height);
    if(grid.empty())return result;
    const auto indexed=indexedMinimap(grid,result.width,result.height,result.width,result.height,palette);
    result.rgba.resize(indexed.size()*4);
    for(size_t i=0;i<indexed.size();++i)std::copy(palette.rgba[indexed[i]],palette.rgba[indexed[i]]+4,result.rgba.begin()+i*4);
    return result;
}

void generateMinimaps(ta::tnt::Map& map, ta::terrain::Compositor& comp,
                      const ta::gaf::Palette& pal, std::shared_ptr<const std::atomic_bool> stop) {
    if (map.blocksX <= 0 || map.blocksY <= 0) return;
    std::vector<uint8_t> grid;
    if (map.tileCount > 0 && map.tileGfx.size() >= size_t(map.tileCount) * 1024) {
        // A TA map's own 8-bit tiles: each block's colour is its tile's mean
        // through the game palette.
        std::vector<std::array<uint8_t,3>> mean(size_t(map.tileCount));
        for (size_t t = 0; t < mean.size(); ++t) {
            uint32_t r = 0, g = 0, b = 0;
            for (size_t i = 0; i < 1024; ++i) {
                const uint8_t* c = pal.rgba[map.tileGfx[t * 1024 + i]];
                r += c[0]; g += c[1]; b += c[2];
            }
            mean[t] = {uint8_t(r / 1024), uint8_t(g / 1024), uint8_t(b / 1024)};
        }
        grid.assign(size_t(map.blocksX) * map.blocksY * 3, 0);
        for (size_t i = 0; i < size_t(map.blocksX) * map.blocksY && i < map.tileIndex.size(); ++i) {
            const uint16_t t = map.tileIndex[i];
            if (t < mean.size()) std::copy(mean[t].begin(), mean[t].end(), grid.begin() + std::ptrdiff_t(i * 3));
        }
    } else {
        grid = blockColourGrid(map, comp, stop);
    }
    if(stop && stop->load())throw std::runtime_error("Save cancelled");

    if (map.tileCount > 0) {
        // Total Annihilation (a map carrying its own tiles): a 252x252 picture,
        // the map aspect-fitted from the top-left and the rest padded, as in
        // the shipped .tnt files.
        const int longSide = std::max(map.blocksX, map.blocksY);
        const int cw = std::max(1, map.blocksX * 252 / longSide), ch = std::max(1, map.blocksY * 252 / longSide);
        const auto fitted = indexedMinimap(grid, map.blocksX, map.blocksY, cw, ch, pal);
        map.minimapW = 252; map.minimapH = 252;
        map.minimap.assign(size_t(252) * 252, 0);
        for (int y = 0; y < ch; ++y)
            std::copy_n(fitted.begin() + std::ptrdiff_t(y) * cw, cw, map.minimap.begin() + std::ptrdiff_t(y) * 252);
        return;
    }
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

} // namespace ta::terrain
