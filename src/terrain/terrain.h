#pragma once

#include "gaf/gaf.h"
#include "tnt/tnt.h"
#include "util/jpeg.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::terrain {

// Composites TNT map terrain out of the map's OWN tile library: each 32px block
// names a 32x32 8-bit indexed tile stored inside the .tnt, which is coloured
// through the game palette (palettes/PALETTE.PAL).
//
// Kingdoms instead kept terrain art in a shared archive as content-addressed
// truecolour JPGs and had maps reference them by key. That made a map cheap but
// useless without its matching terrain archive; TA's maps are self-contained,
// which is why a TA map is megabytes and a Kingdoms one is not.
//
// `jpeg::Image` stays the output type despite no JPEG being involved: it is just
// this codebase's plain RGBA image struct, and every consumer already takes one.

class Compositor {
public:
    explicit Compositor(const hpi::Vfs& vfs);

    // Render the whole map at full resolution (blocksX*32 x blocksY*32 px).
    jpeg::Image renderMap(const tnt::Map& map);

    // Render one 32px block into `dst` (RGBA, dstW px wide) at (dx, dy).
    // Thread-safe: the decoded-tile cache is guarded, so background chunk/minimap
    // builders and the main thread may composite concurrently.
    void renderBlock(const tnt::Map& map, int bx, int by,
                     std::vector<uint8_t>& dst, int dstW, int dx, int dy);

    // Decode (or fetch the cached) RGBA for one tile of `map`'s library. The
    // returned reference stays valid for the Compositor's lifetime (cache_ is a
    // std::map -- node-stable across inserts). Thread-safe. Throws if the index
    // is not in the library. Used by the tile-atlas renderer to upload each
    // tile once rather than re-expanding it per block.
    const jpeg::Image& tileImage(const tnt::Map& map, int index) {
        std::lock_guard<std::mutex> lk(mu_);
        return tile(map, index);
    }

    // The game palette these tiles are coloured through.
    const gaf::Palette& palette() const { return pal_; }

private:
    const jpeg::Image& tile(const tnt::Map& map, int index);

    const hpi::Vfs* vfs_ = nullptr;
    gaf::Palette pal_{};
    std::mutex mu_;                        // guards cache_ (see renderBlock)
    std::map<int, jpeg::Image> cache_;     // tile index -> expanded RGBA (lazy)
};

} // namespace ta::terrain
