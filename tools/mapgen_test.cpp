// mapgen_test [<install-dir>]: the random-map generator (tnt/mapgen.cpp, mapgen_ta.cpp).
//
// Asset-free: the "~gen1~" recipe round-trips every field, sanitize clamps, a malformed
// or other-version id is refused by parseMapId (decodeMapId gives defaults),
// friendlyLabel reads "Random WxH NP <World>", and generate refuses data without TA
// terrain sections.
// With an install: every world builds; the map's size matches the params; the starts
// are in bounds and distinct; ore features are placed (but on Metal, which has none); equal seeds give identical
// maps and different seeds different ones.
#include "tnt/mapgen.h"
#include "hpi/hpi.h"

#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
}

uint64_t hash(const ta::mapgen::Result& r) {
    uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](uint64_t n) { for (int i = 0; i < 8; ++i) { h ^= (n >> (i * 8)) & 255; h *= 1099511628211ULL; } };
    const auto& m = r.map;
    mix(uint64_t(m.width)); mix(uint64_t(m.height)); mix(uint64_t(m.seaLevel));
    for (auto n : m.heights) mix(n);
    for (auto n : m.features) mix(n);
    for (auto n : m.tileIndex) mix(n);
    for (auto n : m.tileGfx) mix(n);
    for (const auto& s : m.featureNames) { for (unsigned char c : s) mix(c); mix(0); }
    for (auto [x, z] : r.starts) { mix(uint64_t(x)); mix(uint64_t(z)); }
    return h;
}

void recipes() {
    using namespace ta::mapgen;
    Params p;
    p.seed = 0x0123456789abcdefULL; p.name = "Test Map"; p.mapType = Moon;
    p.widthCells = 12 * 32; p.heightCells = 20 * 32; p.players = 5;
    p.treeDensity = 1; p.rockDensity = 2; p.manaDensity = 3; p.waterDensity = 4; p.layout = 5;
    p.reliefDensity = 6; p.manaScatter = 7; p.plateauDensity = 8; p.mountainDensity = 9; p.townDensity = 10;
    const std::string id = encodeMapId(p);
    check(isGeneratedMapId(id), "an encoded id is a generated-map id");
    check(!isGeneratedMapId("Ashap Plateau"), "a map name is not a generated-map id");
    const Params q = parseMapId(id);
    check(q.seed == p.seed && q.name == p.name && q.mapType == p.mapType && q.widthCells == p.widthCells &&
          q.heightCells == p.heightCells && q.players == p.players && q.treeDensity == 1 && q.rockDensity == 2 &&
          q.manaDensity == 3 && q.waterDensity == 4 && q.layout == 5 && q.reliefDensity == 6 && q.manaScatter == 7 &&
          q.plateauDensity == 8 && q.mountainDensity == 9 && q.townDensity == 10,
          "encode/decode round-trips every recipe field");
    check(encodeMapId(q) == id, "a decoded recipe re-encodes to the same id");
    // A version-10 id written before the Kingdoms generator was removed still parses.
    const std::string old = "~gen1~0a00f0a981b8b476a2cb010008000808ffffff67ff03000000004a616e6b204d617a65207631";
    bool parsed = true;
    try { (void)parseMapId(old); } catch (const std::exception&) { parsed = false; }
    check(parsed, "an existing version-10 id parses");

    bool refused = false;
    try { (void)parseMapId("~gen1~09000100000000000000038001800108ffffffffff0004"); }
    catch (const std::runtime_error&) { refused = true; }
    check(refused, "parseMapId refuses another recipe version");
    refused = false;
    try { (void)parseMapId("~gen1~zz"); } catch (const std::runtime_error&) { refused = true; }
    check(refused, "parseMapId refuses a malformed id");
    const Params d = decodeMapId("~gen1~zz");
    check(d.mapType == Green && d.players == 2, "decodeMapId gives defaults for a malformed id");

    Params wild;
    wild.mapType = 200; wild.widthCells = 5; wild.heightCells = 9000; wild.players = 1;
    wild.name = "  ab/c\\d<e>  ";
    const Params s = sanitize(wild);
    check(s.mapType == Green && s.widthCells == 8 * 32 && s.heightCells == 64 * 32 && s.players == 2,
          "sanitize clamps the world, the sides and the players");
    check(s.name == "abcde", "sanitize keeps only safe name characters, trimmed");
    Params many; many.players = 12;
    check(sanitize(many).players == 8, "sanitize caps the players at 8");

    Params label; label.widthCells = label.heightCells = 8 * 32; label.players = 4; label.mapType = Green;
    check(friendlyLabel(label) == "Random 8x8 4P Green", "friendlyLabel reads \"Random 8x8 4P Green\": " + friendlyLabel(label));
    label.mapType = Lava; label.widthCells = 16 * 32; label.players = 2;
    check(friendlyLabel(label) == "Random 16x8 2P Lava", "friendlyLabel names the world: " + friendlyLabel(label));
    label.name = "Named";
    check(friendlyLabel(label) == "Named", "a named recipe is labelled by its name");
    check(std::string(worldTitle(Archipelago)) == "ARCHIPELAGO" && std::string(worldKey(Metal)) == "metal",
          "world titles and keys are TA's");

    ta::hpi::Vfs empty;
    bool threw = false;
    try { (void)generate(Params{}, empty); }
    catch (const std::runtime_error& e) { threw = std::string(e.what()).find("sections") != std::string::npos; }
    check(threw, "generate refuses data with no TA terrain sections, saying why");
}

void generated(const char* install) {
    using namespace ta::mapgen;
    const auto vfs = ta::hpi::mountRetailRoot(install);
    check(taSectionsAvailable(vfs), "the install has TA terrain sections");
    check(!assetPaths(vfs).empty(), "the generator's assets are listed for the data hash");
    for (uint8_t world = 0; world < kMapTypes; ++world) {
        const std::string tag = std::string(worldTitle(world)) + ": ";
        Params p;
        p.mapType = world; p.seed = 1000 + world; p.players = uint8_t(2 + world);
        p.widthCells = 12 * 32; p.heightCells = 8 * 32;
        Result r;
        try { r = generate(p, vfs); }
        catch (const std::exception& e) { check(false, tag + "generate threw: " + e.what()); continue; }
        const auto& m = r.map;
        check(m.width == p.widthCells && m.height == p.heightCells, tag + "map size matches the params");
        check(m.blocksX == m.width / 2 && m.blocksY == m.height / 2, tag + "block grid matches the cell grid");
        check(m.heights.size() == size_t(m.width) * m.height && m.features.size() == m.heights.size() &&
              m.tileIndex.size() == size_t(m.blocksX) * m.blocksY, tag + "planes are sized to the map");
        check(m.tileCount > 0 && m.tileGfx.size() == size_t(m.tileCount) * 1024, tag + "the map carries its tiles");
        bool tilesOk = true;
        for (auto t : m.tileIndex) tilesOk = tilesOk && t < m.tileCount;
        check(tilesOk, tag + "every block names one of the map's tiles");
        check(r.starts.size() == p.players, tag + "one start per player");
        std::set<std::pair<int, int>> distinct;
        for (auto [x, z] : r.starts) {
            check(x >= 0 && z >= 0 && x < m.width && z < m.height, tag + "start in bounds");
            distinct.insert({x, z});
        }
        check(distinct.size() == r.starts.size(), tag + "starts are distinct");
        int ore = 0;
        for (auto f : m.features) if (f != 0xFFFF) { ++ore; check(f < m.featureNames.size(), tag + "feature index names a feature"); }
        // Metal ships no indestructible ore feature (features/metal has pipes and towers,
        // reclaimable), so its maps carry none.
        if (world != Metal) check(ore > 0 && !m.featureNames.empty(), tag + "ore features are placed");
        std::printf("%s%dx%d cells, %d tiles, %d ore, %zu starts\n", tag.c_str(), m.width, m.height, m.tileCount, ore,
                    r.starts.size());

        const uint64_t first = hash(r);
        check(first == hash(generate(p, vfs)), tag + "equal seeds build identical maps");
        check(first == hash(generate(parseMapId(encodeMapId(p)), vfs)), tag + "the encoded id builds the same map");
        Params other = p; other.seed += 1;
        check(first != hash(generate(other, vfs)), tag + "another seed builds another map");
    }
    // The largest size and most players still build.
    Params big; big.widthCells = big.heightCells = 64 * 32; big.players = 8; big.mapType = Green;
    try {
        const auto r = generate(big, vfs);
        check(r.map.width == 64 * 32 && r.starts.size() == 8, "a 64x64 8-player map builds");
    } catch (const std::exception& e) { check(false, std::string("64x64 generate threw: ") + e.what()); }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        recipes();
        if (argc > 1) generated(argv[1]);
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    if (failures) { std::printf("mapgen: %d failure(s)\n", failures); return 1; }
    std::printf("PASS: mapgen%s\n", argc > 1 ? " (TA data)" : "");
    return 0;
}
