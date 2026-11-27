#include "tnt/mapgen.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace ta::mapgen {

const char* worldKey(uint8_t world) {
    static const char* keys[kMapTypes] = {"greenworld", "mars", "archipelago", "metal", "moon", "lava"};
    return keys[world < kMapTypes ? world : 0];
}

const char* worldTitle(uint8_t world) {
    static const char* titles[kMapTypes] = {"GREEN", "MARS", "ARCHIPELAGO", "METAL", "MOON", "LAVA"};
    return titles[world < kMapTypes ? world : 0];
}

bool lavaWorld(uint8_t world) { return world == Lava; }

Params sanitize(Params p) {
    std::string name;
    for (unsigned char c : p.name) {
        if (name.size() == 24) break;
        // Safe both as an archive path and as an unquoted OTA value.
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_') name += char(c);
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    const auto first = name.find_first_not_of(' ');
    p.name = first == std::string::npos ? std::string{} : name.substr(first);
    if (p.mapType >= kMapTypes) p.mapType = Green;
    // Section multiples, 8..64 sections per side.
    auto fix = [](uint16_t v) -> uint16_t { return uint16_t(std::clamp(int(v) / 32, 8, 64) * 32); };
    p.widthCells = fix(p.widthCells);
    p.heightCells = fix(p.heightCells);
    p.players = uint8_t(std::clamp<int>(p.players, 2, kMaxPlayers));
    return p;
}

Result generate(const Params& raw, const ta::hpi::Vfs& input) {
    ta::hpi::Vfs vfs(&input, true);
    if (!taSectionsAvailable(vfs))
        throw std::runtime_error("random maps need Total Annihilation terrain sections (sections/*.sct), "
                                 "which this game data does not have");
    return generateTa(sanitize(raw), vfs);
}

// ---- mapId carrier: "~gen1~" + hex of the packed Params -----------------------

namespace {
constexpr const char* kMagic = "~gen1~";

void put16(std::string& b, uint16_t v) { b.push_back(char(v)); b.push_back(char(v >> 8)); }
void put64(std::string& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(char(v >> (8 * i))); }
std::string toHex(const std::string& raw) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(raw.size() * 2);
    for (unsigned char c : raw) { s.push_back(H[c >> 4]); s.push_back(H[c & 15]); }
    return s;
}
int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool isGeneratedMapId(const std::string& id) {
    return id.size() >= 6 && id.compare(0, 6, kMagic) == 0;
}

std::string encodeMapId(const Params& pin) {
    Params p = sanitize(pin);
    std::string b;
    put16(b, Params{}.formatVer);   // only the current layout is written
    put64(b, p.seed);
    b.push_back(char(p.mapType));
    put16(b, p.widthCells); put16(b, p.heightCells);
    b.push_back(char(p.players));
    b.push_back(char(p.treeDensity));
    b.push_back(char(p.rockDensity));
    b.push_back(char(p.manaDensity));
    b.push_back(char(p.waterDensity));
    b.push_back(char(p.reliefDensity));
    b.push_back(char(p.layout));
    b.push_back(char(p.manaScatter));
    b.push_back(char(p.plateauDensity));   // v10 kits, before the variable-length name
    b.push_back(char(p.mountainDensity));
    b.push_back(char(p.townDensity));
    b += p.name;
    return std::string(kMagic) + toHex(b);
}

namespace {
// The fixed fields encodeMapId writes, before the 0..24-byte name.
constexpr size_t kFixedBytes = 26;

// The Params of a well-formed id: the magic, the current version and its exact payload.
std::optional<Params> tryDecode(const std::string& id) {
    if (!isGeneratedMapId(id) || (id.size() - 6) % 2) return std::nullopt;
    const size_t bytes = (id.size() - 6) / 2;
    if (bytes < kFixedBytes || bytes > kFixedBytes + 24) return std::nullopt;
    std::string raw;
    for (size_t i = 6; i + 1 < id.size(); i += 2) {
        int hi = hexNib(id[i]), lo = hexNib(id[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        raw.push_back(char((hi << 4) | lo));
    }
    auto u8 = [&](size_t o) { return uint8_t(raw[o]); };
    auto u16 = [&](size_t o) { return uint16_t(u8(o) | (u8(o + 1) << 8)); };
    Params p;
    if (u16(0) != p.formatVer) return std::nullopt;
    uint64_t s = 0; for (int i = 0; i < 8; ++i) s |= uint64_t(u8(2 + i)) << (8 * i);
    p.seed = s;
    p.mapType = u8(10);
    p.widthCells = u16(11); p.heightCells = u16(13);
    p.players = u8(15);
    p.treeDensity = u8(16);
    p.rockDensity = u8(17);
    p.manaDensity = u8(18);
    p.waterDensity = u8(19);
    p.reliefDensity = u8(20);
    p.layout = u8(21);
    p.manaScatter = u8(22);
    p.plateauDensity = u8(23); p.mountainDensity = u8(24); p.townDensity = u8(25);
    p.name = raw.substr(kFixedBytes);
    return sanitize(p);
}
}  // namespace

Params decodeMapId(const std::string& id) {
    if (auto p = tryDecode(id)) return *p;
    return sanitize(Params{});
}

Params parseMapId(const std::string& id) {
    if (auto p = tryDecode(id)) return *p;
    throw std::runtime_error("unknown random-map recipe (this build makes version " +
                             std::to_string(Params{}.formatVer) + "); generate a new map");
}

std::string friendlyLabel(const Params& pin) {
    Params p = sanitize(pin);
    if (!p.name.empty()) return p.name;
    static const char* kNames[kMapTypes] = {"Green", "Mars", "Archipelago", "Metal", "Moon", "Lava"};
    int u = p.widthCells / 32, v = p.heightCells / 32;
    // ASCII only: the lobby draws this with the 5x7 block font, which has no glyph
    // for a middot and would render each byte of one as a blank.
    return "Random " + std::to_string(u) + "x" + std::to_string(v) + " " +
           std::to_string(int(p.players)) + "P " + kNames[p.mapType];
}

}  // namespace ta::mapgen
