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

constexpr size_t kHeaderBytes = 64;      // 16 u32 words
constexpr size_t kFeatNameRec = 132;     // u32 index + 128-byte name

} // namespace

Map Map::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<uint8_t> d(std::filesystem::file_size(file));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return load(d, file.string());
}

Map Map::load(const std::vector<uint8_t>& d, const std::string& origin) {
    need(d, 0, kHeaderBytes, "TNT header");
    if (u32(&d[0]) != 0x2000)
        throw std::runtime_error(origin + ": not a TA TNT (version != 0x2000)");

    Map m;
    m.width = int(u32(&d[4]));
    m.height = int(u32(&d[8]));
    if (m.width <= 0 || m.height <= 0 || m.width > 65535 || m.height > 65535)
        throw std::runtime_error(origin + ": implausible map size");
    // The tile grid is half the cell grid, rounded UP: a map with an odd cell
    // dimension still needs a tile covering the last half-row. Retail sizes are
    // always even, but truncating here would under-read the plane if one is not.
    m.blocksX = (m.width + 1) / 2;
    m.blocksY = (m.height + 1) / 2;

    uint32_t pTiles = u32(&d[12]);
    uint32_t pAttr = u32(&d[16]);
    uint32_t pGfx = u32(&d[20]);
    m.numTiles = int(u32(&d[24]));
    uint32_t featCount = u32(&d[28]);
    uint32_t pFeatNames = u32(&d[32]);
    m.seaLevel = int(u32(&d[36]));
    uint32_t pMinimap = u32(&d[40]);

    size_t cells = size_t(m.width) * m.height;
    size_t blocks = size_t(m.blocksX) * m.blocksY;

    need(d, pTiles, blocks * 2, "tile indices");
    m.tiles.resize(blocks);
    for (size_t i = 0; i < blocks; ++i)
        m.tiles[i] = uint16_t(d[pTiles + i * 2] | (d[pTiles + i * 2 + 1] << 8));

    // MapAttr: { u8 height; u16 feature; u8 unused }. The u16 is unaligned at
    // byte 1, so it is assembled by hand rather than cast.
    need(d, pAttr, cells * 4, "map attributes");
    m.heights.resize(cells);
    m.features.resize(cells);
    for (size_t i = 0; i < cells; ++i) {
        const uint8_t* a = &d[pAttr + i * 4];
        m.heights[i] = a[0];
        m.features[i] = uint16_t(a[1] | (a[2] << 8));
    }

    if (m.numTiles < 0 || size_t(m.numTiles) > (d.size() / kTileBytes) + 1)
        throw std::runtime_error(origin + ": implausible tile count");
    need(d, pGfx, size_t(m.numTiles) * kTileBytes, "tile graphics");
    m.tileGfx.assign(d.begin() + pGfx, d.begin() + pGfx + size_t(m.numTiles) * kTileBytes);

    // Feature names: 132-byte records, name at +4. A map with no features omits
    // the table entirely (count 0), which is not an error.
    if (pFeatNames && featCount && featCount < 65536 &&
        uint64_t(pFeatNames) + uint64_t(featCount) * kFeatNameRec <= d.size()) {
        m.featureNames.reserve(featCount);
        for (uint32_t i = 0; i < featCount; ++i) {
            const char* nm = reinterpret_cast<const char*>(&d[pFeatNames + i * kFeatNameRec + 4]);
            m.featureNames.emplace_back(nm, strnlen(nm, kFeatNameRec - 4));
        }
    }

    if (pMinimap && pMinimap + 8 <= d.size()) {
        m.minimapW = int(u32(&d[pMinimap]));
        m.minimapH = int(u32(&d[pMinimap + 4]));
        size_t n = size_t(m.minimapW) * m.minimapH;
        need(d, pMinimap + 8, n, "minimap");
        m.minimap.assign(d.begin() + pMinimap + 8, d.begin() + pMinimap + 8 + n);
    }
    return m;
}

std::vector<uint8_t> Map::save() const {
    std::vector<uint8_t> d;
    auto putU32 = [&](uint32_t x) {
        d.push_back(uint8_t(x)); d.push_back(uint8_t(x >> 8));
        d.push_back(uint8_t(x >> 16)); d.push_back(uint8_t(x >> 24));
    };

    uint32_t hdr[16] = {0};
    hdr[0] = 0x2000;
    hdr[1] = uint32_t(width);
    hdr[2] = uint32_t(height);
    hdr[6] = uint32_t(numTiles);
    hdr[7] = uint32_t(featureNames.size());
    hdr[9] = uint32_t(seaLevel);
    hdr[11] = 1;                 // matches every retail map read so far
    d.assign(kHeaderBytes, 0);

    size_t cells = size_t(width) * height;
    size_t blocks = size_t(blocksX) * blocksY;

    // Physical order as retail writes it: tiles, MapAttr, graphics, names, minimap.
    hdr[3] = uint32_t(d.size());
    for (size_t i = 0; i < blocks; ++i) {
        uint16_t t = i < tiles.size() ? tiles[i] : 0;
        d.push_back(uint8_t(t)); d.push_back(uint8_t(t >> 8));
    }
    // Retail pads the tile plane -- and ONLY the tile plane -- up to a 16-byte
    // boundary; every other section abuts the next exactly. Checked across all 95
    // maps shipped with the Commander Pack: 16-byte alignment matches every one,
    // where 4-byte alignment matches 58. Without this a load->save round trip is
    // a byte or two short of the original and cannot be compared for identity.
    d.resize((d.size() + 15) / 16 * 16, 0);

    hdr[4] = uint32_t(d.size());
    for (size_t i = 0; i < cells; ++i) {
        uint16_t f = i < features.size() ? features[i] : kNoFeature;
        d.push_back(i < heights.size() ? heights[i] : 0);
        d.push_back(uint8_t(f)); d.push_back(uint8_t(f >> 8));
        d.push_back(0);
    }

    hdr[5] = uint32_t(d.size());
    d.insert(d.end(), tileGfx.begin(), tileGfx.end());
    // Pad to the declared tile count so a short library cannot make the reader
    // walk off the end of the plane it was told to expect.
    d.resize(size_t(hdr[5]) + size_t(numTiles) * kTileBytes, 0);

    hdr[8] = uint32_t(d.size());
    for (size_t i = 0; i < featureNames.size(); ++i) {
        putU32(uint32_t(i));
        char name[kFeatNameRec - 4] = {0};
        std::strncpy(name, featureNames[i].c_str(), sizeof name - 1);
        d.insert(d.end(), name, name + sizeof name);
    }

    hdr[10] = uint32_t(d.size());
    putU32(uint32_t(minimapW));
    putU32(uint32_t(minimapH));
    size_t mm = size_t(minimapW) * minimapH;
    d.insert(d.end(), minimap.begin(), minimap.begin() + std::min(minimap.size(), mm));
    d.resize(size_t(hdr[10]) + 8 + mm, 0);

    for (int i = 0; i < 16; ++i) {
        d[i * 4 + 0] = uint8_t(hdr[i]);        d[i * 4 + 1] = uint8_t(hdr[i] >> 8);
        d[i * 4 + 2] = uint8_t(hdr[i] >> 16);  d[i * 4 + 3] = uint8_t(hdr[i] >> 24);
    }
    return d;
}

} // namespace ta::tnt
