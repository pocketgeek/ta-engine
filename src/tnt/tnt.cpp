#include "tnt/tnt.h"

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
    need(d, 0, 52, "TNT header");
    if (u32(&d[0]) != 0x4000)
        throw std::runtime_error(origin + ": not a TAK TNT (version != 0x4000)");

    Map m;
    m.width = int(u32(&d[4]));
    m.height = int(u32(&d[8]));
    m.seaLevel = int(u32(&d[12]));
    m.blocksX = m.width / 2;
    m.blocksY = m.height / 2;

    uint32_t pHeights = u32(&d[16]);
    uint32_t pFeatures = u32(&d[20]);
    uint32_t pKeys = u32(&d[32]);
    uint32_t pCols = u32(&d[36]);
    uint32_t pRows = u32(&d[40]);
    uint32_t pMinimap = u32(&d[44]);

    size_t cells = size_t(m.width) * m.height;
    size_t blocks = size_t(m.blocksX) * m.blocksY;

    need(d, pHeights, cells, "heights");
    m.heights.assign(d.begin() + pHeights, d.begin() + pHeights + cells);

    need(d, pFeatures, cells * 2, "features");
    m.features.resize(cells);
    for (size_t i = 0; i < cells; ++i)
        m.features[i] = uint16_t(d[pFeatures + i * 2] | (d[pFeatures + i * 2 + 1] << 8));

    need(d, pKeys, blocks * 4, "tile keys");
    m.tileKeys.resize(blocks);
    for (size_t i = 0; i < blocks; ++i) m.tileKeys[i] = u32(&d[pKeys + i * 4]);

    need(d, pCols, blocks, "tile columns");
    m.tileCols.assign(d.begin() + pCols, d.begin() + pCols + blocks);
    need(d, pRows, blocks, "tile rows");
    m.tileRows.assign(d.begin() + pRows, d.begin() + pRows + blocks);

    // Feature-name table: header words 6/7 = pointer + count;
    // 132-byte records with the name at offset +4.
    uint32_t pFeatNames = u32(&d[24]);
    uint32_t featCount = u32(&d[28]);
    if (pFeatNames && featCount && featCount < 4096 &&
        uint64_t(pFeatNames) + uint64_t(featCount) * 132 <= d.size()) {
        for (uint32_t i = 0; i < featCount; ++i) {
            const char* nm = reinterpret_cast<const char*>(&d[pFeatNames + i * 132 + 4]);
            m.featureNames.emplace_back(nm, strnlen(nm, 64));
        }
    }

    if (pMinimap && pMinimap + 8 <= d.size()) {
        m.minimapW = int(u32(&d[pMinimap]));
        m.minimapH = int(u32(&d[pMinimap + 4]));
        size_t n = size_t(m.minimapW) * m.minimapH;
        need(d, pMinimap + 8, n, "minimap");
        m.minimap.assign(d.begin() + pMinimap + 8, d.begin() + pMinimap + 8 + n);
    }

    // Large overview minimap (header word 12): same {u32 w, u32 h, w*h bytes}
    // layout as the small one. Preserved so a load->save round-trips it verbatim
    // (the game shows it as the in-game map overview).
    uint32_t pOverview = u32(&d[48]);
    if (pOverview && pOverview + 8 <= d.size()) {
        m.overviewW = int(u32(&d[pOverview]));
        m.overviewH = int(u32(&d[pOverview + 4]));
        size_t n = size_t(m.overviewW) * m.overviewH;
        if (pOverview + 8 + n <= d.size())
            m.overview.assign(d.begin() + pOverview + 8, d.begin() + pOverview + 8 + n);
        else { m.overviewW = m.overviewH = 0; }
    }
    return m;
}

std::vector<uint8_t> Map::save() const {
    std::vector<uint8_t> d;
    auto putU32 = [&](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
        v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
    };
    // 52-byte (13 dword) header placeholder; pointers back-patched as we go.
    uint32_t hdr[13] = {0};
    hdr[0] = 0x4000;
    hdr[1] = uint32_t(width);
    hdr[2] = uint32_t(height);
    hdr[3] = uint32_t(seaLevel);
    d.assign(52, 0);
    auto patch = [&](int word, uint32_t val) { hdr[word] = val; };

    size_t cells = size_t(width) * height;
    size_t blocks = size_t(blocksX) * blocksY;

    // heights (u8/cell, row-major)
    patch(4, uint32_t(d.size()));
    for (size_t i = 0; i < cells; ++i) d.push_back(i < heights.size() ? heights[i] : 0);

    // features (u16/cell, row-major)
    patch(5, uint32_t(d.size()));
    for (size_t i = 0; i < cells; ++i) {
        uint16_t v = i < features.size() ? features[i] : 0xFFFF;
        d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8));
    }

    // feature-name table: count x 132-byte records {u32 seq index, 128-byte name}
    patch(6, uint32_t(d.size()));
    patch(7, uint32_t(featureNames.size()));
    for (size_t i = 0; i < featureNames.size(); ++i) {
        putU32(d, uint32_t(i));
        char name[128] = {0};
        std::strncpy(name, featureNames[i].c_str(), sizeof name - 1);
        d.insert(d.end(), name, name + sizeof name);
    }

    // tile keys (u32/block), columns (u8/block), rows (u8/block), row-major
    patch(8, uint32_t(d.size()));
    for (size_t i = 0; i < blocks; ++i) putU32(d, i < tileKeys.size() ? tileKeys[i] : 0);
    patch(9, uint32_t(d.size()));
    for (size_t i = 0; i < blocks; ++i) d.push_back(i < tileCols.size() ? tileCols[i] : 0);
    patch(10, uint32_t(d.size()));
    for (size_t i = 0; i < blocks; ++i) d.push_back(i < tileRows.size() ? tileRows[i] : 0);

    // small minimap {u32 w, u32 h, w*h bytes}
    patch(11, uint32_t(d.size()));
    putU32(d, uint32_t(minimapW)); putU32(d, uint32_t(minimapH));
    d.insert(d.end(), minimap.begin(),
             minimap.begin() + std::min(minimap.size(), size_t(minimapW) * minimapH));
    while (d.size() < size_t(hdr[11]) + 8 + size_t(minimapW) * minimapH) d.push_back(0);

    // large overview minimap {u32 w, u32 h, w*h bytes}
    patch(12, uint32_t(d.size()));
    putU32(d, uint32_t(overviewW)); putU32(d, uint32_t(overviewH));
    d.insert(d.end(), overview.begin(),
             overview.begin() + std::min(overview.size(), size_t(overviewW) * overviewH));
    while (d.size() < size_t(hdr[12]) + 8 + size_t(overviewW) * overviewH) d.push_back(0);

    // Back-patch the header.
    for (int i = 0; i < 13; ++i) {
        d[i * 4 + 0] = uint8_t(hdr[i]); d[i * 4 + 1] = uint8_t(hdr[i] >> 8);
        d[i * 4 + 2] = uint8_t(hdr[i] >> 16); d[i * 4 + 3] = uint8_t(hdr[i] >> 24);
    }
    return d;
}

} // namespace ta::tnt
