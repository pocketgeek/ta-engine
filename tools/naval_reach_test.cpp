// A mobile builder may only place a site it can get within its build distance of
// (World::buildSiteReachable, the client's placement verdict). Every walking
// ground builder whose menu holds something that must stand in water (a yard with
// water cells, a ship, a Zhon water creature) stands on island A of a test map:
//
//   x cells  0..40   island A (the builder)
//           40..100  sea
//          100..256  land B, holding a closed lake at 150..190 x 40..90
//
// A water site against A's shore is reachable; one out in the open sea, beyond
// build distance of every shore cell, is not; the lake inside B is not either (no
// land route from A) but is for a builder standing on B. A flyer reaches anything.
// The sim agrees: the reachable order starts a site, the unreachable one is
// dropped without one (retail MobileBuild's CANNOTREACHCONSTRUCTION outcome).
// With --legion the same runs in Legion pathfinding mode on a placement plane.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const bool legion = argc == 3 && std::string_view(argv[2]) == "--legion";
    if (argc == 3 && !legion) return 2;
    auto vfs = tak::hpi::mountRetailRoot(argv[1]);
    int failures = 0, cases = 0;
    auto check = [&](bool ok, const char* label, const std::string& what) {
        std::printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", what.c_str(), label);
        if (!ok) ++failures;
    };
    constexpr int W = 256, H = 128;
    std::vector<uint8_t> heights(size_t(W) * H, 70);
    for (int z = 0; z < H; ++z)
        for (int x = 0; x < W; ++x)
            if ((x >= 40 && x < 100) || (x >= 150 && x < 190 && z >= 40 && z < 90))
                heights[size_t(z) * W + x] = 10;
    for (bool crusades : {false, true}) {
        tak::sim::TypeRegistry registry;
        tak::sim::setupRegistry(registry, vfs, crusades);
        auto world = [&](tak::sim::World& w) {
            w.setTerrain(heights, W, H, 64);
            w.buildNavClasses(registry);
            w.setPathService(true);   // the async path service, as in a game
            if (legion) {
                w.setPathfindingMode(tak::sim::PathfindingMode::Legion);
                w.setMapPlacementFeatures(std::vector<uint16_t>(size_t(W) * H, 0xffff), {});
            }
            w.player(0).mana = 100000;
        };
        const tak::sim::UnitType* flyer = nullptr;
        for (const auto& [id, t] : registry.types())
            if (t.isBuilder && t.canFly && !t.isStructure() && (!flyer || id < flyer->id)) flyer = &t;
        // Shipyards are ground-domain structures whose yard map wants water ('w');
        // a Zhon producer's water creature is a water-domain unit.
        auto needsWater = [](const tak::sim::UnitType& t) {
            if (t.domain == tak::sim::UnitType::Domain::Water) return true;
            return t.isStructure() && std::any_of(t.yardMap.begin(), t.yardMap.end(),
                                                  [](char c) { return c == 'w' || c == 'W'; });
        };
        std::vector<std::pair<std::string, std::string>> pairs;   // builder, water structure
        for (const auto& [id, t] : registry.types()) {
            if (!t.isBuilder || t.isStructure() || t.canFly ||
                t.domain != tak::sim::UnitType::Domain::Ground) continue;
            std::string pick;
            for (const auto& s : registry.buildable(id))
                if (const auto* st = registry.find(s); st && needsWater(*st) && (pick.empty() || s < pick)) pick = s;
            if (!pick.empty()) pairs.push_back({id, pick});
        }
        std::sort(pairs.begin(), pairs.end());
        check(!pairs.empty(), "a land builder with a water structure exists", crusades ? "crusades" : "retail");
        for (const auto& [bid, sid] : pairs) {
            const auto* bt = registry.find(bid);
            const auto* st = registry.find(sid);
            const std::string what = std::string(crusades ? "crusades " : "retail ") + bid + " -> " + sid;
            const float z = 64 * 16;
            // Footprint against A's shore; far out at sea, past any shore's reach; the lake.
            const float nearX = float(40 * 16 + st->footX * 8);
            const float farX = float(70 * 16);
            const float lakeX = float(150 * 16 + st->footX * 8);   // against B's lake shore
            const float aX = 20 * 16, bX = 130 * 16;
            ++cases;
            {
                tak::sim::World w; world(w);
                const int a = w.spawn(bt, aX, z, 0.f, 0);
                const int b = w.spawn(bt, bX, z, 0.f, 0);
                check(w.canPlace(st, nearX, z, 0) && w.canPlace(st, farX, z, 0) && w.canPlace(st, lakeX, z, 0),
                      "every test site passes terrain placement", what);
                check(w.buildSiteReachable(a, st, nearX, z), "site against the builder's shore is reachable", what);
                check(!w.buildSiteReachable(a, st, farX, z), "open-sea site beyond build distance is refused", what);
                check(!w.buildSiteReachable(a, st, lakeX, z), "lake behind the sea is refused from the island", what);
                check(w.buildSiteReachable(b, st, lakeX, z), "the same lake is reachable from its own shore", what);
                if (flyer) {
                    const int f = w.spawn(flyer, aX, z, 0.f, 0);
                    check(w.buildSiteReachable(f, st, farX, z), "a flying builder reaches the open sea", what);
                }
            }
            // The sim's own outcome for the orders the client now allows and refuses.
            for (bool reachable : {true, false}) {
                tak::sim::World w; world(w);
                const int a = w.spawn(bt, aX, z, 0.f, 0);
                const float sx = reachable ? nearX : lakeX;
                w.queueBuild(a, st, sx, z, false);
                bool started = false;
                for (int tick = 0; tick < 3600 && !started; ++tick) {
                    w.tick(1.f / 30.f);
                    for (const auto& u : w.units())
                        started |= u.alive() && u.type == st;
                    if (!reachable && w.unit(a)->orders.empty()) break;
                }
                if (reachable) check(started, "the sim starts the reachable site", what);
                else check(!started && w.unit(a)->orders.empty(),
                           "the sim drops the unreachable order without a site", what);
            }
        }
    }
    std::printf("%d builder/structure pairs, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
