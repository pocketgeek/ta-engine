#include "util/png.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tak::png {

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
    // Not for an empty chunk: zlib's crc32 with a null buffer returns its INITIAL
    // value, so IEND (no data, null data()) used to get a CRC of 0.
    if (!data.empty()) crc = crc32(crc, data.data(), uInt(data.size()));
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

void writeRgb(const std::filesystem::path& file, int width, int height,
              const std::vector<uint8_t>& rgb) {
    if (width <= 0 || height <= 0 || rgb.size() != size_t(width) * height * 3)
        throw std::runtime_error("png::writeRgb: buffer size mismatch");
    std::ofstream out(file, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open " + file.string());
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(width));
    be32(ihdr, uint32_t(height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // color type: RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);

    z_stream z{};
    if (deflateInit(&z, 6) != Z_OK) throw std::runtime_error("png::writeRgb: zlib init failed");
    std::vector<uint8_t> idat(size_t(1) << 20), row(size_t(width) * 3 + 1);
    auto pump = [&](int flush) {
        do {
            z.next_out = idat.data();
            z.avail_out = uInt(idat.size());
            const int rc = deflate(&z, flush);
            if (rc == Z_STREAM_ERROR) { deflateEnd(&z); throw std::runtime_error("png::writeRgb: zlib failed"); }
            const size_t have = idat.size() - z.avail_out;
            if (have) chunk(out, "IDAT", std::vector<uint8_t>(idat.begin(), idat.begin() + std::ptrdiff_t(have)));
        } while (z.avail_out == 0);
    };
    for (int y = 0; y < height; ++y) {
        // Filter 1 (Sub): terrain art compresses far better than with none.
        row[0] = 1;
        const uint8_t* src = rgb.data() + size_t(y) * width * 3;
        for (size_t i = 0; i < size_t(width) * 3; ++i)
            row[i + 1] = uint8_t(src[i] - (i >= 3 ? src[i - 3] : 0));
        z.next_in = row.data();
        z.avail_in = uInt(row.size());
        pump(Z_NO_FLUSH);
    }
    pump(Z_FINISH);
    deflateEnd(&z);
    chunk(out, "IEND", {});
    if (!out) throw std::runtime_error("write failed: " + file.string());
}

} // namespace tak::png
