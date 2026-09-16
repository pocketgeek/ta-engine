#include "gaf/gaf.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ta::gaf {

namespace {

uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
int16_t s16(const uint8_t* p) { return int16_t(u16(p)); }

struct FrameHeader {
    uint16_t w, h;
    int16_t xoff, yoff;
    uint8_t transparency, encoding;
    uint16_t numSubframes;
    uint32_t dataPtr;
};

FrameHeader frameHeader(const std::vector<uint8_t>& d, uint32_t off) {
    if (uint64_t(off) + 24 > d.size()) throw std::runtime_error("frame header out of range");
    FrameHeader f;
    f.w = u16(&d[off]);
    f.h = u16(&d[off + 2]);
    f.xoff = s16(&d[off + 4]);
    f.yoff = s16(&d[off + 6]);
    f.transparency = d[off + 8];
    f.encoding = d[off + 9];
    f.numSubframes = u16(&d[off + 10]);
    f.dataPtr = u32(&d[off + 16]);
    return f;
}

void need(const std::vector<uint8_t>& d, uint64_t off, uint64_t n, const char* what) {
    if (off + n > d.size()) throw std::runtime_error(std::string(what) + " out of range");
}

void putIndexed(Frame& fr, int x, int y, uint8_t idx, uint8_t transparency, const Palette& pal) {
    if (x < 0 || y < 0 || x >= fr.width || y >= fr.height) return;
    uint8_t* px = &fr.rgba[(size_t(y) * fr.width + x) * 4];
    if (idx == transparency) return;  // leave pixel as-is (transparent)
    px[0] = pal.rgba[idx][0];
    px[1] = pal.rgba[idx][1];
    px[2] = pal.rgba[idx][2];
    px[3] = 255;
}

Frame decodeSingle(const std::vector<uint8_t>& d, const FrameHeader& h, const Palette& pal) {
    Frame fr;
    fr.width = h.w;
    fr.height = h.h;
    fr.xoff = h.xoff;
    fr.yoff = h.yoff;
    fr.rgba.assign(size_t(h.w) * h.h * 4, 0);

    switch (h.encoding) {
        case 0: {  // raw 8-bit indexed
            need(d, h.dataPtr, uint64_t(h.w) * h.h, "raw pixel data");
            for (int y = 0; y < h.h; ++y)
                for (int x = 0; x < h.w; ++x)
                    putIndexed(fr, x, y, d[h.dataPtr + size_t(y) * h.w + x], h.transparency, pal);
            break;
        }
        case 1: {  // RLE 8-bit indexed, per scanline
            uint32_t pos = h.dataPtr;
            for (int y = 0; y < h.h; ++y) {
                need(d, pos, 2, "RLE line header");
                uint16_t lineBytes = u16(&d[pos]);
                pos += 2;
                need(d, pos, lineBytes, "RLE line data");
                uint32_t p = pos;
                uint32_t end = pos + lineBytes;
                int x = 0;
                while (p < end) {
                    uint8_t code = d[p++];
                    if (code & 1) {
                        x += code >> 1;                       // transparent run
                    } else if (code & 2) {
                        uint8_t v = d[p++];                   // repeat run
                        for (int n = (code >> 2) + 1; n-- > 0;)
                            putIndexed(fr, x++, y, v, h.transparency, pal);
                    } else {
                        for (int n = (code >> 2) + 1; n-- > 0;)  // literal run
                            putIndexed(fr, x++, y, d[p++], h.transparency, pal);
                    }
                }
                pos = end;
            }
            break;
        }
        case 4: {  // raw ARGB4444
            need(d, h.dataPtr, uint64_t(h.w) * h.h * 2, "4444 pixel data");
            for (size_t i = 0; i < size_t(h.w) * h.h; ++i) {
                uint16_t v = u16(&d[h.dataPtr + i * 2]);
                uint8_t* px = &fr.rgba[i * 4];
                px[0] = uint8_t(((v >> 8) & 0xF) * 17);
                px[1] = uint8_t(((v >> 4) & 0xF) * 17);
                px[2] = uint8_t((v & 0xF) * 17);
                px[3] = uint8_t(((v >> 12) & 0xF) * 17);
            }
            break;
        }
        case 5: {  // raw ARGB1555
            need(d, h.dataPtr, uint64_t(h.w) * h.h * 2, "1555 pixel data");
            for (size_t i = 0; i < size_t(h.w) * h.h; ++i) {
                uint16_t v = u16(&d[h.dataPtr + i * 2]);
                uint8_t* px = &fr.rgba[i * 4];
                px[0] = uint8_t(((v >> 10) & 0x1F) * 255 / 31);
                px[1] = uint8_t(((v >> 5) & 0x1F) * 255 / 31);
                px[2] = uint8_t((v & 0x1F) * 255 / 31);
                px[3] = (v & 0x8000) ? 255 : 0;
            }
            break;
        }
        default:
            throw std::runtime_error("unknown GAF encoding " + std::to_string(h.encoding));
    }
    return fr;
}

Frame decodeFrame(const std::vector<uint8_t>& d, uint32_t off, const Palette& pal) {
    FrameHeader h = frameHeader(d, off);
    if (h.numSubframes == 0) return decodeSingle(d, h, pal);

    // Composite: subframes are aligned so their anchor points coincide.
    Frame fr;
    fr.width = h.w;
    fr.height = h.h;
    fr.xoff = h.xoff;
    fr.yoff = h.yoff;
    fr.rgba.assign(size_t(h.w) * h.h * 4, 0);
    // Some shipped TAFs carry junk in the HIGH byte of the subframe-count field --
    // the cannonball sprites (cannbsm/cannbmed/cannblg), the hurricane loop and the
    // big dust cloud all read 65280 (0xFF00) where a healthy frame reads 0, while
    // their ordinary single-frame data pointer is perfectly valid. Retail clearly
    // ignores it; reading the count strictly made us throw away the art for 23
    // weapons plus a storm effect. No real GAF composites hundreds of subframes, so
    // an implausible count means "not a composite" -- decode the plain frame.
    if (h.numSubframes > 255 ||
        uint64_t(h.dataPtr) + uint64_t(h.numSubframes) * 4 > d.size())
        return decodeSingle(d, h, pal);
    for (uint16_t i = 0; i < h.numSubframes; ++i) {
        Frame sub = decodeFrame(d, u32(&d[h.dataPtr + i * 4]), pal);
        int ox = h.xoff - sub.xoff;
        int oy = h.yoff - sub.yoff;
        for (int y = 0; y < sub.height; ++y) {
            int ty = y + oy;
            if (ty < 0 || ty >= fr.height) continue;
            for (int x = 0; x < sub.width; ++x) {
                int tx = x + ox;
                if (tx < 0 || tx >= fr.width) continue;
                const uint8_t* s = &sub.rgba[(size_t(y) * sub.width + x) * 4];
                if (s[3] == 0) continue;
                std::memcpy(&fr.rgba[(size_t(ty) * fr.width + tx) * 4], s, 4);
            }
        }
    }
    return fr;
}

} // namespace

Palette Palette::load(const std::filesystem::path& palFile) {
    std::ifstream in(palFile, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + palFile.string());
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return fromBytes(d, palFile.string());
}

Palette Palette::fromBytes(const std::vector<uint8_t>& d, const std::string& origin) {
    Palette pal{};

    if (!d.empty() && d[0] == 0x0A) {
        // PCX: TAK ships palettes as 1x1 PCX files. The 256-color palette is
        // the last 768 bytes, preceded by a 0x0C marker byte.
        if (d.size() < 769 || d[d.size() - 769] != 0x0C)
            throw std::runtime_error(origin + ": PCX without VGA palette");
        const uint8_t* p = d.data() + d.size() - 768;
        for (int i = 0; i < 256; ++i) {
            pal.rgba[i][0] = p[i * 3];
            pal.rgba[i][1] = p[i * 3 + 1];
            pal.rgba[i][2] = p[i * 3 + 2];
            pal.rgba[i][3] = 255;
        }
        return pal;
    }

    // Raw .pal: 256 x 4 bytes (R,G,B,x).
    if (d.size() < 1024)
        throw std::runtime_error(origin + ": short palette");
    for (int i = 0; i < 256; ++i) {
        pal.rgba[i][0] = d[i * 4];
        pal.rgba[i][1] = d[i * 4 + 1];
        pal.rgba[i][2] = d[i * 4 + 2];
        pal.rgba[i][3] = 255;
    }
    return pal;
}

std::vector<Sequence> load(const std::filesystem::path& file, const Palette& pal,
                           int transparentIndex) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<uint8_t> d(std::filesystem::file_size(file));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return load(d, pal, transparentIndex, file.string());
}

std::vector<Sequence> load(const std::vector<uint8_t>& d, const Palette& pal,
                           int transparentIndex, const std::string& origin) {
    need(d, 0, 12, "GAF header");
    if (u32(&d[0]) != 0x00010100)
        throw std::runtime_error(origin + ": not a GAF/TAF (bad version)");
    uint32_t numEntries = u32(&d[4]);
    need(d, 12, uint64_t(numEntries) * 4, "entry pointers");

    std::vector<Sequence> out;
    out.reserve(numEntries);
    for (uint32_t i = 0; i < numEntries; ++i) {
        uint32_t e = u32(&d[12 + i * 4]);
        need(d, e, 40, "entry header");
        Sequence seq;
        uint16_t numFrames = u16(&d[e]);
        const char* nm = reinterpret_cast<const char*>(&d[e + 8]);
        seq.name.assign(nm, strnlen(nm, 32));
        need(d, e + 40, uint64_t(numFrames) * 8, "frame pointers");
        for (uint16_t f = 0; f < numFrames; ++f) {
            Frame fr = decodeFrame(d, u32(&d[e + 40 + f * 8]), pal);
            // The second dword of each frame record is the display duration in
            // 30Hz engine ticks (retail advances feature anims once per ~30fps
            // frame, holding each GAF frame for this count).
            uint32_t delay = u32(&d[e + 40 + f * 8 + 4]);
            fr.delayTicks = int(std::clamp<uint32_t>(delay, 1, 300));
            if (transparentIndex >= 0) {
                const uint8_t* key = pal.rgba[transparentIndex];
                for (size_t px = 0; px + 3 < fr.rgba.size(); px += 4)
                    if (fr.rgba[px] == key[0] && fr.rgba[px + 1] == key[1] &&
                        fr.rgba[px + 2] == key[2])
                        fr.rgba[px + 3] = 0;
            }
            seq.frames.push_back(std::move(fr));
        }
        out.push_back(std::move(seq));
    }
    return out;
}

} // namespace ta::gaf
