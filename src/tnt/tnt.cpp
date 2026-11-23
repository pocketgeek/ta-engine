#include "tnt/tnt.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ta::tnt {

namespace {

uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

void need(const std::vector<uint8_t>& d, uint64_t off, uint64_t n, const char* what) {
    if (off + n > d.size()) throw std::runtime_error(std::string(what) + " out of range");
}

} // namespace

Map Map::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<uint8_t> d(std::filesystem::file_size(file));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return load(d, file.string());
}

Map Map::load(const std::vector<uint8_t>& d, const std::string& origin) {
    need(d, 0, 64, "TNT header");
    uint32_t version = u32(&d[0]);
    if (version != 0x2000) {
        char buf[64];
        std::snprintf(buf, sizeof buf, ": unsupported TNT version 0x%08x", version);
        throw std::runtime_error(origin + buf);
    }
    Map m;
    m.width = int(u32(&d[0x04]));
    m.height = int(u32(&d[0x08]));
    if (m.width <= 0 || m.height <= 0 || m.width > 8192 || m.height > 8192)
        throw std::runtime_error(origin + ": bad TNT dimensions");
    m.blocksX = m.width / 2;
    m.blocksY = m.height / 2;
    uint32_t pTileMap = u32(&d[0x0c]), pAttr = u32(&d[0x10]), pGfx = u32(&d[0x14]);
    uint32_t tiles = u32(&d[0x18]), featCount = u32(&d[0x1c]), pFeat = u32(&d[0x20]);
    m.seaLevel = int(u32(&d[0x24]));
    uint32_t pMini = u32(&d[0x28]), flags = u32(&d[0x2c]);

    size_t cells = size_t(m.width) * m.height;
    size_t blocks = size_t(m.blocksX) * m.blocksY;

    need(d, pTileMap, blocks * 2, "tile map");
    m.tileIndex.resize(blocks);
    for (size_t i = 0; i < blocks; ++i)
        m.tileIndex[i] = uint16_t(d[pTileMap + i * 2] | (d[pTileMap + i * 2 + 1] << 8));

    need(d, pAttr, cells * 4, "cell attributes");
    m.heights.resize(cells);
    m.features.resize(cells);
    m.cellExtra.resize(cells);
    for (size_t i = 0; i < cells; ++i) {
        const uint8_t* a = &d[pAttr + i * 4];
        m.heights[i] = a[0];
        m.features[i] = uint16_t(a[1] | (a[2] << 8));
        m.cellExtra[i] = a[3];
    }

    if (tiles > 65536) throw std::runtime_error(origin + ": bad tile count");
    need(d, pGfx, uint64_t(tiles) * 1024, "tile graphics");
    m.tileCount = int(tiles);
    m.tileGfx.assign(d.begin() + pGfx, d.begin() + pGfx + size_t(tiles) * 1024);
    for (auto& t : m.tileIndex)
        if (t >= tiles) t = 0;   // out-of-range tile: draw tile 0 rather than read past

    if (featCount > 65535) throw std::runtime_error(origin + ": bad feature count");
    need(d, pFeat, uint64_t(featCount) * 132, "feature names");
    for (uint32_t i = 0; i < featCount; ++i) {
        const char* nm = reinterpret_cast<const char*>(&d[pFeat + i * 132 + 4]);
        m.featureNames.emplace_back(nm, strnlen(nm, 128));
    }

    if ((flags & 1) && pMini && uint64_t(pMini) + 8 <= d.size()) {
        m.minimapW = int(u32(&d[pMini]));
        m.minimapH = int(u32(&d[pMini + 4]));
        size_t n = size_t(m.minimapW) * m.minimapH;
        need(d, pMini + 8, n, "minimap");
        m.minimap.assign(d.begin() + pMini + 8, d.begin() + pMini + 8 + n);
    }
    return m;
}

std::vector<uint8_t> Map::save() const {
    std::vector<uint8_t> d(64, 0);
    auto put = [&](uint32_t x) {
        d.push_back(uint8_t(x)); d.push_back(uint8_t(x >> 8));
        d.push_back(uint8_t(x >> 16)); d.push_back(uint8_t(x >> 24));
    };
    uint32_t hdr[16] = {0};
    hdr[0] = 0x2000;
    hdr[1] = uint32_t(width);
    hdr[2] = uint32_t(height);
    size_t cells = size_t(width) * height;
    size_t blocks = size_t(blocksX) * blocksY;
    hdr[3] = uint32_t(d.size());
    for (size_t i = 0; i < blocks; ++i) {
        uint16_t t = i < tileIndex.size() ? tileIndex[i] : 0;
        d.push_back(uint8_t(t)); d.push_back(uint8_t(t >> 8));
    }
    hdr[4] = uint32_t(d.size());
    for (size_t i = 0; i < cells; ++i) {
        uint16_t f = i < features.size() ? features[i] : 0xFFFF;
        d.push_back(i < heights.size() ? heights[i] : 0);
        d.push_back(uint8_t(f)); d.push_back(uint8_t(f >> 8));
        d.push_back(i < cellExtra.size() ? cellExtra[i] : 0);
    }
    hdr[5] = uint32_t(d.size());
    hdr[6] = uint32_t(tileCount);
    d.insert(d.end(), tileGfx.begin(), tileGfx.begin() + std::min(tileGfx.size(), size_t(tileCount) * 1024));
    while (d.size() < size_t(hdr[5]) + size_t(tileCount) * 1024) d.push_back(0);
    hdr[7] = uint32_t(featureNames.size());
    hdr[8] = uint32_t(d.size());
    for (size_t i = 0; i < featureNames.size(); ++i) {
        put(uint32_t(i));
        char name[128] = {0};
        std::strncpy(name, featureNames[i].c_str(), sizeof name - 1);
        d.insert(d.end(), name, name + sizeof name);
    }
    hdr[9] = uint32_t(seaLevel);
    if (minimapW > 0 && minimapH > 0) {
        hdr[10] = uint32_t(d.size());
        hdr[11] = 1;
        put(uint32_t(minimapW)); put(uint32_t(minimapH));
        size_t n = size_t(minimapW) * minimapH;
        d.insert(d.end(), minimap.begin(), minimap.begin() + std::min(minimap.size(), n));
        while (d.size() < size_t(hdr[10]) + 8 + n) d.push_back(0);
    }
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 4; ++k) d[size_t(i) * 4 + k] = uint8_t(hdr[i] >> (8 * k));
    return d;
}

} // namespace ta::tnt
