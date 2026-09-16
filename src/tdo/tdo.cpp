#include "tdo/tdo.h"

#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace ta::tdo {

namespace {

constexpr float kScale = 1.0f / 65536.0f;

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("3DO read out of range");
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (uint32_t(d[off + 3]) << 24);
}

int32_t s32(const std::vector<uint8_t>& d, size_t off) { return int32_t(u32(d, off)); }

std::string cstr(const std::vector<uint8_t>& d, size_t off) {
    if (off >= d.size()) throw std::runtime_error("3DO string out of range");
    const char* s = reinterpret_cast<const char*>(&d[off]);
    return std::string(s, strnlen(s, d.size() - off));
}

// Parse an object and its sibling chain as a list.
std::vector<Object> parseChain(const std::vector<uint8_t>& d, size_t off, int depth);

Object parseOne(const std::vector<uint8_t>& d, size_t off, int depth) {
    if (depth > 64) throw std::runtime_error("3DO tree too deep");
    if (u32(d, off) != 1) throw std::runtime_error("3DO object version != 1");

    Object obj;
    uint32_t numVerts = u32(d, off + 4);
    uint32_t numPrims = u32(d, off + 8);
    obj.x = float(s32(d, off + 16)) * kScale;
    obj.y = float(s32(d, off + 20)) * kScale;
    obj.z = float(s32(d, off + 24)) * kScale;
    uint32_t offName = u32(d, off + 28);
    uint32_t offVerts = u32(d, off + 36);
    uint32_t offPrims = u32(d, off + 40);
    uint32_t offChild = u32(d, off + 48);

    if (offName) obj.name = cstr(d, offName);

    obj.vertices.reserve(numVerts * 3);
    for (uint32_t i = 0; i < numVerts; ++i) {
        obj.vertices.push_back(float(s32(d, offVerts + i * 12)) * kScale);
        obj.vertices.push_back(float(s32(d, offVerts + i * 12 + 4)) * kScale);
        obj.vertices.push_back(float(s32(d, offVerts + i * 12 + 8)) * kScale);
    }

    for (uint32_t i = 0; i < numPrims; ++i) {
        size_t p = offPrims + i * 32;
        Primitive prim;
        prim.colorIndex = u32(d, p);
        uint32_t n = u32(d, p + 4);
        uint32_t offIdx = u32(d, p + 12);
        uint32_t offTex = u32(d, p + 16);
        for (uint32_t k = 0; k < n; ++k) {
            size_t io = offIdx + k * 2;
            if (io + 2 > d.size()) throw std::runtime_error("3DO index out of range");
            prim.indices.push_back(uint16_t(d[io] | (d[io + 1] << 8)));
        }
        if (offTex) prim.texture = cstr(d, offTex);
        obj.primitives.push_back(std::move(prim));
    }

    if (offChild)
        obj.children = parseChain(d, offChild, depth + 1);
    return obj;
}

std::vector<Object> parseChain(const std::vector<uint8_t>& d, size_t off, int depth) {
    std::vector<Object> out;
    while (off) {
        uint32_t sibling = u32(d, off + 44);
        out.push_back(parseOne(d, off, depth));
        off = sibling;
    }
    return out;
}

void collectTextures(const Object& o, std::set<std::string>& out) {
    for (const auto& p : o.primitives)
        if (!p.texture.empty()) out.insert(p.texture);
    for (const auto& c : o.children) collectTextures(c, out);
}

} // namespace

Model load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    std::vector<uint8_t> d(std::filesystem::file_size(file));
    in.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
    return load(d);
}

Model load(const std::vector<uint8_t>& d) {
    Model m;
    m.root = parseOne(d, 0, 0);
    return m;
}

std::vector<std::string> Model::textures() const {
    std::set<std::string> s;
    collectTextures(root, s);
    return {s.begin(), s.end()};
}

} // namespace ta::tdo
