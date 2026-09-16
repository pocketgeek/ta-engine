#include "util/png.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ta::png {

namespace {

void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void chunk(std::ofstream& out, const char type[4], const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    be32(hdr, uint32_t(data.size()));
    out.write(reinterpret_cast<const char*>(hdr.data()), 4);
    out.write(type, 4);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    crc = crc32(crc, data.data(), uInt(data.size()));
    std::vector<uint8_t> tail;
    be32(tail, uint32_t(crc));
    out.write(reinterpret_cast<const char*>(tail.data()), 4);
}

} // namespace

void write(const std::filesystem::path& file, int width, int height,
           const std::vector<uint8_t>& rgba) {
    if (rgba.size() != size_t(width) * height * 4)
        throw std::runtime_error("png::write: buffer size mismatch");

    // Raw stream: one filter byte (0) per scanline.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * width * 4,
                   rgba.begin() + size_t(y + 1) * width * 4);
    }

    uLongf compLen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), uLong(raw.size()), 9) != Z_OK)
        throw std::runtime_error("png::write: zlib compress failed");
    comp.resize(compLen);

    std::ofstream out(file, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open " + file.string());
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.write(reinterpret_cast<const char*>(sig), 8);

    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(width));
    be32(ihdr, uint32_t(height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // color type: RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", comp);
    chunk(out, "IEND", {});
    if (!out) throw std::runtime_error("write failed: " + file.string());
}

} // namespace ta::png
