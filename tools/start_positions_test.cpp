// Fixed start locations follow retail: the SEATED players take the map's starts
// in slot order, and an empty seat takes none. A human in slot 1 and an AI in
// slot 8 therefore start at StartPos1 and StartPos2, not 1 and 8.
//
// Established in KINGDOMS.icd, statically and by emulation (docs/retail-engine.md,
// "Start positions"): the skirmish spawn at 0x4d2e70 and the multiplayer host's
// start table at 0x4ea950 both walk the ten player records (one per battle-room
// row) and give the k-th occupied one the k-th start. This pins that rule so a
// report like "slot 8 should start at 8" is not "fixed" into a non-retail game.
//
// That game is what BY SLOT (net::kStartsBySlot, an engine extension) is for: slot N
// opens at StartPos N whichever seats are empty. A slot past the map's last start
// takes the lowest start no seated slot claims by its own number, then an extra
// spot, so the same seats always give the same layout (sim::startSpotsFor).
#include "hpi/hpi.h"
#include "net/matchconfig.h"
#include "sim/matchsetup.h"
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
using namespace tak;

static void check(bool ok, const std::string& text) { if (!ok) throw std::runtime_error(text); }

static std::vector<uint8_t> mapBytes() {
    tnt::Map map; map.width = map.height = 128; map.blocksX = map.blocksY = 64;
    map.heights.assign(size_t(128) * 128, 100); map.features.assign(size_t(128) * 128, 0xffff);
    const auto blocks = size_t(map.blocksX) * map.blocksY;
    map.tileKeys.resize(blocks); map.tileCols.resize(blocks); map.tileRows.resize(blocks);
    map.minimapW = map.minimapH = map.overviewW = map.overviewH = 1;
    map.minimap = {0}; map.overview = {0}; return map.save();
}

// Eight starts, numbered down the west edge (1-4) and up the east edge (5-8), as
// a "4 vs 4" map lays out its teams. Stored out of order: the number decides.
static const int kStart[8][2] = {{8, 8}, {8, 40}, {8, 72}, {8, 104},
                                 {120, 104}, {120, 72}, {120, 40}, {120, 8}};

// The map's .ota with StartPos1..count (8: maps/starts, 4: maps/four, the west edge).
static std::string ota(int count) {
    std::string s = "[GlobalHeader]\n{\nmissionname=Starts;\nnumplayers=" + std::to_string(count) +
                    ";\n[Map Data]\n{\n[specials]\n{\n";
    for (int n : {5, 1, 8, 2, 7, 3, 6, 4})
        if (n <= count)
            s += "[special" + std::to_string(n) + "]\n{\nspecialwhat=StartPos" + std::to_string(n) +
                 ";\nXPos=" + std::to_string(kStart[n - 1][0]) + ";\nZPos=" + std::to_string(kStart[n - 1][1]) + ";\n}\n";
    return s + "}\n}\n}\n";
}

struct Game {
    uint8_t mode = net::kStartsFixed;     // GameOptions::randomStarts
    uint32_t seed = 1;
    const char* map = "maps/starts.tnt";
    bool oneTeam = false;                 // every seat on team 1 (default: each its own)
};

// Which StartPos (1-based) the Monarch of each seated slot got, in slot order;
// 0 = not on any StartPos (an extra spot the map does not have).
static std::vector<int> startsFor(const hpi::Vfs& vfs, std::initializer_list<std::pair<int, uint8_t>> seats,
                                  Game game = {}) {
    net::SlotInfo slots[net::kMaxSlots];
    for (auto [slot, type] : seats) {
        slots[slot].type = type;                  // 1 human, 2 AI
        slots[slot].faction = uint8_t(slot % 5);
        slots[slot].team = game.oneTeam ? 0 : uint8_t(slot);
    }
    net::GameOptions o; o.randomStarts = game.mode;
    // The builder every peer, the referee and replay playback use.
    auto cfg = net::matchConfigFor(o, slots, game.seed);
    cfg.vfs = &vfs; cfg.mapPath = game.map;
    sim::TypeRegistry reg; sim::World world;
    const auto spots = sim::setupMatch(world, reg, cfg);
    std::vector<int> got;
    for (const auto& [x, z] : spots) {
        int n = 0;
        for (int k = 0; k < 8; ++k)
            if (x == float(kStart[k][0] * 16) && z == float(kStart[k][1] * 16)) n = k + 1;
        got.push_back(n);
    }
    return got;
}

static std::string show(const std::vector<int>& v) {
    std::string s;
    for (int n : v) s += (s.empty() ? "" : ",") + std::to_string(n);
    return s;
}

int main() try {
    auto files = std::make_shared<hpi::Vfs::Files>();
    (*files)["maps/starts.tnt"] = mapBytes();
    const auto text = ota(8); (*files)["maps/starts.ota"] = {text.begin(), text.end()};
    (*files)["maps/four.tnt"] = mapBytes();
    const auto four = ota(4); (*files)["maps/four.ota"] = {four.begin(), four.end()};
    hpi::Vfs vfs; vfs.setMapFiles(files);
    const auto parsed = sim::parseStartPositions(vfs, "maps/starts.tnt");
    check(parsed.size() == 8 && parsed[0] == std::make_pair(128.f, 128.f) &&
          parsed[7] == std::make_pair(1920.f, 128.f), "the starts are not read in StartPos order");
    check(sim::parseStartPositions(vfs, "maps/four.tnt").size() == 4, "the four-start map does not have four starts");

    // The reported game: a human in slot 1 and an AI in slot 8, slots 2-7 open.
    // Retail's 0x4d2e70 spawns them at StartPos1 and StartPos2.
    auto got = startsFor(vfs, {{0, 1}, {7, 2}});
    check(got == std::vector<int>{1, 2}, "human slot 1 + AI slot 8 should take starts 1,2 as retail does, got " + show(got));
    // Gaps anywhere collapse the same way: the k-th seated slot takes start k.
    got = startsFor(vfs, {{0, 1}, {2, 2}, {5, 2}, {7, 2}});
    check(got == std::vector<int>{1, 2, 3, 4}, "seats 1,3,6,8 should take starts 1-4, got " + show(got));
    // An empty first seat does not reserve StartPos1 either.
    got = startsFor(vfs, {{3, 2}, {6, 1}});
    check(got == std::vector<int>{1, 2}, "seats 4,7 should take starts 1,2, got " + show(got));
    // With every seat taken, slot N is at StartPos N.
    got = startsFor(vfs, {{0, 1}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}, {6, 2}, {7, 2}});
    check(got == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8}, "a full table should be slot N at start N, got " + show(got));
    // Fixed starts do not depend on the match seed.
    check(startsFor(vfs, {{0, 1}, {7, 2}}, {.seed = 99}) == std::vector<int>{1, 2}, "fixed starts moved with the seed");
    // Random starts still give each seated player a distinct real start.
    got = startsFor(vfs, {{0, 1}, {7, 2}}, {.mode = net::kStartsRandom, .seed = 12345});
    check(got.size() == 2 && got[0] && got[1] && got[0] != got[1], "random starts lost a start, got " + show(got));
    std::puts("PASS fixed starts: seated slots take StartPos1..N in slot order (retail 0x4d2e70 / 0x4ea950)");

    // BY SLOT: the same reported game opens at StartPos1 and StartPos8.
    const Game bySlot{.mode = net::kStartsBySlot};
    got = startsFor(vfs, {{0, 1}, {7, 2}}, bySlot);
    check(got == std::vector<int>{1, 8}, "by slot: human slot 1 + AI slot 8 should take starts 1,8, got " + show(got));
    got = startsFor(vfs, {{0, 1}, {2, 2}, {5, 2}, {7, 2}}, bySlot);
    check(got == std::vector<int>{1, 3, 6, 8}, "by slot: seats 1,3,6,8 should take starts 1,3,6,8, got " + show(got));
    got = startsFor(vfs, {{3, 2}, {6, 1}}, bySlot);
    check(got == std::vector<int>{4, 7}, "by slot: seats 4,7 should take starts 4,7, got " + show(got));
    got = startsFor(vfs, {{0, 1}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}, {6, 2}, {7, 2}}, bySlot);
    check(got == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8}, "by slot: a full table should be slot N at start N, got " + show(got));
    // Neither the seed nor the teams move anyone.
    got = startsFor(vfs, {{0, 1}, {7, 2}}, {.mode = net::kStartsBySlot, .seed = 99});
    check(got == std::vector<int>{1, 8}, "by slot: starts moved with the seed, got " + show(got));
    got = startsFor(vfs, {{0, 1}, {7, 2}}, {.mode = net::kStartsBySlot, .oneTeam = true});
    check(got == std::vector<int>{1, 8}, "by slot: allies in slots 1 and 8 should still take starts 1,8, got " + show(got));
    // A slot past the map's last start takes the lowest start no seated slot claims by
    // its own number, then an extra spot (0 here): never another player's start.
    const Game bySlotFour{.mode = net::kStartsBySlot, .map = "maps/four.tnt"};
    got = startsFor(vfs, {{0, 1}, {1, 2}, {7, 2}}, bySlotFour);
    check(got == std::vector<int>{1, 2, 3}, "by slot, 4 starts: seats 1,2,8 should take starts 1,2,3, got " + show(got));
    got = startsFor(vfs, {{2, 1}, {7, 2}}, bySlotFour);
    check(got == std::vector<int>{3, 1}, "by slot, 4 starts: seats 3,8 should take starts 3,1, got " + show(got));
    got = startsFor(vfs, {{0, 1}, {1, 2}, {2, 2}, {3, 2}, {4, 2}}, bySlotFour);
    check(got == std::vector<int>{1, 2, 3, 4, 0}, "by slot, 4 starts: seats 1-5 should take starts 1-4 and an extra spot, got " + show(got));
    // The rule itself: a map without starts gives every seat an extra spot, in order.
    std::vector<sim::MatchSlot> seats(8);
    seats[1].used = seats[6].used = true;
    const auto none = sim::startSpotsFor(seats, 0, sim::StartMode::BySlot);
    check(none == std::vector<int>{-1, 0, -1, -1, -1, -1, 1, -1}, "by slot, no starts: seats 2,7 should take extra spots 0,1");
    check(sim::startSpotsFor(seats, 8, sim::StartMode::Fixed) == std::vector<int>{-1, 0, -1, -1, -1, -1, 1, -1},
          "fixed: seats 2,7 should take spots 0,1");
    std::puts("PASS by slot: slot N takes StartPos N; past the map's last start, the lowest unclaimed one");
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
