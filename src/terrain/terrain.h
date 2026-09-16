#pragma once

#include "tnt/tnt.h"
#include "util/jpeg.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace ta::terrain {

// Composites TNT map terrain from the content-addressed section JPGs the VFS
// resolves as terrain/<hexkey>.jpg (from terrain.hpi). Each 32px map block
// crops (col*32, row*32, 32, 32) out of its keyed JPG.

class Compositor {
public:
    explicit Compositor(const hpi::Vfs& vfs);

    // Render the whole map at full resolution (width*16 x height*16 px).
    jpeg::Image renderMap(const tnt::Map& map);

    // Render one 32px block into `dst` (RGBA, dstW px wide) at (dx, dy).
    // Thread-safe: the decoded-tile cache is guarded, so background chunk/minimap
    // builders and the main thread may composite concurrently.
    void renderBlock(const tnt::Map& map, int bx, int by,
                     std::vector<uint8_t>& dst, int dstW, int dx, int dy);

    // Decode (or fetch the cached) section JPG for `key` and return it. The
    // returned reference stays valid for the Compositor's lifetime (cache_ is a
    // std::map -- node-stable across inserts). Thread-safe. Throws if absent.
    // Used by the tile-atlas terrain renderer to upload each section once.
    const jpeg::Image& sectionImage(uint32_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        return section(key);
    }

private:
    const jpeg::Image& section(uint32_t key);

    const hpi::Vfs* vfs_ = nullptr;
    std::mutex mu_;                           // guards cache_ (see renderBlock)
    std::map<uint32_t, jpeg::Image> cache_;   // key -> decoded section (lazy)
};

} // namespace ta::terrain
