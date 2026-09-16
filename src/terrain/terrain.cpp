#include "terrain/terrain.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ta::terrain {

namespace {
constexpr int kBlock = 32;

// Where retail keeps the shared 256-entry game palette.
constexpr const char* kPalettePath = "palettes/PALETTE.PAL";
}

Compositor::Compositor(const hpi::Vfs& vfs) : vfs_(&vfs) {
    // Load the palette once. A missing palette leaves every entry zeroed, which
    // renders the map black rather than throwing out of a constructor that the
    // client builds as a by-value member long before it has a map to draw.
    try {
        if (vfs_->has(kPalettePath))
            pal_ = gaf::Palette::fromBytes(vfs_->read(kPalettePath), kPalettePath);
    } catch (const std::exception&) {
        // Leave pal_ zeroed; see above.
    }
}

const jpeg::Image& Compositor::tile(const tnt::Map& map, int index) {
    auto it = cache_.find(index);
    if (it != cache_.end()) return it->second;

    const uint8_t* src = map.tile(index);
    if (!src) throw std::runtime_error("tile " + std::to_string(index) + " not in library");

    jpeg::Image img;
    img.width = kBlock;
    img.height = kBlock;
    img.rgba.resize(size_t(kBlock) * kBlock * 4);
    for (int i = 0; i < kBlock * kBlock; ++i) {
        const uint8_t* c = pal_.rgba[src[i]];
        uint8_t* o = &img.rgba[size_t(i) * 4];
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
    }
    return cache_.emplace(index, std::move(img)).first->second;
}

void Compositor::renderBlock(const tnt::Map& map, int bx, int by,
                             std::vector<uint8_t>& dst, int dstW, int dx, int dy) {
    // One coarse lock for lookup+expand+copy: `tile` returns a reference into
    // cache_, so the copy below must not race an insert from another thread.
    std::lock_guard<std::mutex> lk(mu_);
    size_t b = size_t(by) * map.blocksX + bx;
    if (b >= map.tiles.size()) return;
    const jpeg::Image* imgp = nullptr;
    // A tile index the library cannot resolve must not throw out of a chunk
    // worker thread (that would be std::terminate); the block just stays empty.
    try { imgp = &tile(map, map.tiles[b]); } catch (const std::exception&) { return; }

    // Water is just a texture, exactly like land -- retail draws the sea tiles
    // as-is and applies no tint. Height vs. seaLevel drives gameplay, not art.
    for (int y = 0; y < kBlock; ++y) {
        const uint8_t* srow = &imgp->rgba[size_t(y) * kBlock * 4];
        uint8_t* drow = &dst[(size_t(dy + y) * dstW + dx) * 4];
        std::memcpy(drow, srow, size_t(kBlock) * 4);
    }
}

jpeg::Image Compositor::renderMap(const tnt::Map& map) {
    jpeg::Image out;
    out.width = map.blocksX * kBlock;
    out.height = map.blocksY * kBlock;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    for (int by = 0; by < map.blocksY; ++by)
        for (int bx = 0; bx < map.blocksX; ++bx)
            renderBlock(map, bx, by, out.rgba, out.width, bx * kBlock, by * kBlock);
    return out;
}

} // namespace ta::terrain
