// placement_test -- no unit may start on ground it cannot stand on.
//
// This is the THIRD fix in this area, which is why it is now a test rather than a
// measurement pasted into a commit message. The earlier two were: benchmark and
// stress-fill units dropped onto a blind lattice with no terrain check at all
// (~4.5-20.5% of an army starting in a lake or on a cliff), and bodies placed inside
// each other because the overlap check reserved a centre cell instead of a footprint.
// Both were found by looking; neither left anything behind that would notice a
// recurrence.
//
// The one this test was written for: the commander (the Kingdoms Monarch then) was the single spawn in setupMatch
// that never consulted the terrain. Every other body was snapped to ground it fits;
// the Monarch's footprint was merely RESERVED so nothing else snapped onto it, and it
// was then placed at the raw start spot whatever was there. Invisible on a real map,
// whose start positions are authored -- but the benchmark ignores those and spreads
// factions around a COMPUTED ring, and any game with more slots than the map has
// starts fills the shortfall from a second computed ring. Measured on Ulasem Arena at
// 8 players: 2 of 8 Monarchs on ground they did not fit, at every intensity. A Monarch
// cannot walk out of a blocked cell, so it stayed there for the whole match.
//
// SCOPE, so the 8-unit benchmark rows are not mistaken for full coverage: a benchmark
// spawns its army from a RAMP executed during ticks, and those positions are snapped at
// PLAN time inside setupMatch. This test does not tick, so for benchmark cases it checks
// the Monarchs only. The ramp uses the same snapSpawn as the stress fill, which the
// stress cases below do exercise at scale (~3800 and ~2800 bodies) -- so the shared path
// is covered; ticking each benchmark for 1800 ticks would add minutes to buy little.
//
// Needs a retail install, so it registers only with -DTA_TEST_DATA.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "sim/footprint.h"
#include "sim/sides.h"
#include "client/renderframe.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <string>
#include <vector>

using namespace ta;

namespace ta::sim {
struct RetailReplayProbe {
    // Both executors' stages (the stress fill's, the benchmark ramp's at its nominal ticks),
    // in tick order, the stress stage first on a tie.
    static std::vector<BenchStage> plan(const World& world) {
        std::vector<BenchStage> all = world.benchPlan_;
        all.insert(all.end(), world.benchStages_.begin(), world.benchStages_.end());
        std::stable_sort(all.begin(), all.end(), [](const BenchStage& a, const BenchStage& b) { return a.tick < b.tick; });
        return all;
    }
};
}

namespace {
int failures = 0;

// Every configuration that places bodies without an authored position behind it.
struct Case { const char* name; const char* map; int players; int benchmark; bool stress; };

const Case kCases[] = {
    {"benchmark Low",         "Ashap Plateau", 8, 1, false},
    {"benchmark High",        "Ashap Plateau", 8, 3, false},
    {"benchmark Extra Absurd","Ashap Plateau", 8, 6, false},
    {"benchmark cramped",     "Great Divide",  8, 4, false},
    {"stress Ashap",          "Ashap Plateau", 8, 0, true},
    {"stress cramped",        "Great Divide",  8, 0, true},
    // More slots than the map has start positions (Luschinfloggen has six): the
    // shortfall comes from a computed ring too, and is just as unchecked as the
    // benchmark's.
    {"overfilled starts",     "Luschinfloggen", 8, 0, false},
};

// A body stands legally when the nav grid for ITS movement class admits its footprint
// at its cell -- the same question the mover asks, so a unit that fails here is one
// that can never move off the spot it started on.
bool standsLegally(const sim::World& w, const sim::Unit& u) {
    if (!u.type || u.type->canFly) return true;         // flyers are over it all
    const sim::NavGrid& g = w.navFor(u.type);
    if (g.empty()) return true;
    const int foot = std::clamp(std::max(u.type->footX, u.type->footZ), 1, 15);
    return g.fits(int(u.x.toFloat()) / 16, int(u.z.toFloat()) / 16, foot);
}

void runCase(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, const Case& c) {
    sim::World world;
    sim::MatchConfig cfg;
    cfg.vfs = &vfs;
    cfg.mapPath = hpi::findMap(vfs, c.map);
    cfg.benchmark = c.benchmark;
    cfg.stressTest = c.stress;
    cfg.stressImmediate = true;                          // check the bodies, not a plan
    cfg.unitCap = 500;                                   // keep the stress fills quick
    for (int i = 0; i < c.players; ++i) {
        sim::MatchSlot s; s.used = true; s.faction = i % sides::count(); s.team = i;
        cfg.slots.push_back(s);
    }
    sim::setupMatch(world, reg, cfg);

    std::map<std::pair<int,const sim::UnitType*>,int> counts;
    for (const auto& u:world.units()) if (u.alive() && u.type) {
        const int n=++counts[{u.player,u.type}];
        if (u.type->totalAllowed>0 && n>u.type->totalAllowed) {
            std::printf("      player %d exceeds %s totalallowed=%d\n",u.player,
                        u.type->id.c_str(),u.type->totalAllowed);
            ++failures;
        }
    }
    for (const auto& stage:sim::RetailReplayProbe::plan(world))
        for (const auto& spawn:stage.units) {
            const int n=++counts[{spawn.player,spawn.type}];
            if (spawn.type->totalAllowed>0 && n>spawn.type->totalAllowed) {
                std::printf("      benchmark plan exceeds %s totalallowed=%d for player %d\n",
                            spawn.type->id.c_str(),spawn.type->totalAllowed,spawn.player);
                ++failures;
            }
        }

    int total = 0, bad = 0, badMonarch = 0;
    for (const auto& u : world.units()) {
        if (!u.alive() || !u.type) continue;
        ++total;
        if (standsLegally(world, u)) continue;
        ++bad;
        if (u.type->commander || sides::isCommander(u.type->id)) ++badMonarch;
        if (bad <= 3)
            std::printf("      %s at %.0f,%.0f does not fit its cell\n",
                        u.type->id.c_str(), u.x.toFloat(), u.z.toFloat());
    }
    std::printf("  %-24s %4d units, %d unplaceable%s\n", c.name, total, bad,
                badMonarch ? "  (INCLUDING A COMMANDER)" : "");
    if (bad) ++failures;
}

// The staged stress fill (MatchConfig::stressImmediate=false, the game default) must
// place exactly the army the immediate fill places -- same player, type and position for
// every body -- only spread over ticks 1..kStressStageTicks, and must not turn the run
// into a benchmark (commanders stay mortal, no end tick).
using Body = std::tuple<int, std::string, float, float>;
std::vector<Body> bodies(const sim::World& w) {
    std::vector<Body> out;
    for (const auto& u : w.units())
        if (u.alive() && u.type) out.emplace_back(u.player, u.type->id, u.x.toFloat(), u.z.toFloat());
    std::sort(out.begin(), out.end());
    return out;
}

void stagedStressCase(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, int benchmark) {
    auto setup = [&](sim::World& w, bool immediate, int bench) {
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = hpi::findMap(vfs, "Ashap Plateau");
        cfg.stressTest = true;
        cfg.stressImmediate = immediate;
        cfg.benchmark = bench;
        cfg.unitCap = 500;
        for (int i = 0; i < 8; ++i) {
            sim::MatchSlot s; s.used = true; s.faction = i % sides::count(); s.team = i;
            cfg.slots.push_back(s);
        }
        sim::setupMatch(w, reg, cfg);
    };
    sim::World now, staged;
    setup(now, true, benchmark);
    setup(staged, false, benchmark);
    const auto want = bodies(now);
    const size_t opening = bodies(staged).size();
    const auto plan = sim::RetailReplayProbe::plan(staged);
    size_t planned = 0, peak = 0;
    bool ordered = true;
    uint32_t prev = 0, lastStress = 0;
    for (const auto& stage : plan) {
        if (stage.tick < prev) ordered = false;
        prev = stage.tick;
        if (stage.tick <= uint32_t(sim::kStressStageTicks)) {
            planned += stage.units.size();
            peak = std::max(peak, stage.units.size());
            if (!stage.units.empty()) lastStress = stage.tick;
        }
    }
    // Benchmark stages may share the first ticks; count only what the stress fill adds.
    const size_t benchEarly = [&] {
        size_t n = 0;
        for (const auto& stage : sim::RetailReplayProbe::plan(now))
            if (stage.tick <= uint32_t(sim::kStressStageTicks)) n += stage.units.size();
        return n;
    }();
    planned -= benchEarly;
    // Both armies hold fire, so they stand where they were placed: with fire
    // at will a unit of the immediate fill could walk off after an enemy the
    // staged fill has not placed yet.
    auto holdFire = [](sim::World& w) {
        for (const auto& u : w.units())
            if (sim::Unit* m = w.unit(u.id); m && m->alive()) m->fireState = 0;
    };
    for (int i = 0; i <= sim::kStressStageTicks; ++i) {
        holdFire(now); holdFire(staged);
        now.tick(1.f / 30.f);
        staged.tick(1.f / 30.f);
    }
    const bool same = bodies(staged) == bodies(now);
    const bool modeOk = staged.benchmarkMode() == (benchmark != 0);
    const size_t spread = (planned + sim::kStressStageTicks - 1) / sim::kStressStageTicks + 8;
    std::printf("  staged stress%s: opening %zu bodies (immediate %zu), %zu staged over ticks 1..%u, "
                "peak %zu/tick; same army after tick %d: %s\n",
                benchmark ? " + benchmark" : "", opening, want.size(), planned, lastStress, peak,
                sim::kStressStageTicks, same ? "yes" : "NO");
    if (!same || !ordered || !modeOk || opening != 8 || planned + 8 != want.size() ||
        lastStress > uint32_t(sim::kStressStageTicks) || peak > spread + (benchmark ? 8 : 0)) {
        std::printf("      staged stress fill: FAIL (ordered=%d mode=%d)\n", ordered, modeOk);
        ++failures;
    }
}

// A TA shipyard (ARMSY/CORSY: an 8x8 all-water yard, wCCCCCCw...) on a real
// sea map and on controlled terrain. Retail decodes w/C/Y with the water bit
// (docs/retail-data.md 2.4); a wholly water yard is placed by water level.
constexpr const char* kSeaMap = "Coast To Coast";

// The first open-water shipyard site of the map, scanning row-major.
bool findShipyardSite(const sim::World& w, const sim::UnitType* yard, float& sx, float& sz) {
    for (int cz = 2; cz + yard->footZ + 2 < w.mapH(); cz += 2)
        for (int cx = 2; cx + yard->footX + 2 < w.mapW(); cx += 2) {
            const float x = float(cx * 16 + yard->footX * 8), z = float(cz * 16 + yard->footZ * 8);
            if (w.canPlace(yard, x, z)) { sx = x; sz = z; return true; }
        }
    return false;
}

void shipyardPlacementCase(const hpi::Vfs& vfs, const sim::TypeRegistry& reg) {
    const std::string mapPath=hpi::findMap(vfs,kSeaMap);
    if (mapPath.empty()) { std::printf("      %s not found: FAIL\n",kSeaMap); ++failures; return; }
    const auto map=tnt::Map::load(vfs.read(mapPath),mapPath);
    const auto* yard=reg.find("armsy");
    const auto* builder=reg.find("armcs");   // ARM construction ship (builds armsy, 60 px builddistance)
    if (!yard || !builder) { std::printf("      armsy/armcs missing: FAIL\n"); ++failures; return; }

    sim::World world;
    sim::MatchConfig cfg; cfg.vfs=&vfs; cfg.mapPath=mapPath;
    cfg.slots={{true,0,0,1.0f,false,false}};
    sim::setupMatch(world,reg,cfg);

    // Map-backed open-water shipyard site.
    float sx=0,sz=0;
    const bool placeable=findShipyardSite(world,yard,sx,sz);
    std::printf("  %s shipyard open-water canPlace: %s (%.0f,%.0f)\n",kSeaMap,placeable?"PASS":"FAIL",sx,sz);
    if (!placeable) { ++failures; return; }

    // A construction ship beside the site, within its build distance, starts it
    // through the ordinary API.
    const float bx=sx-float(yard->footX*8)-float(builder->footX*8)-8;
    const int bid=world.spawn(builder,bx,sz,0,0);
    const int sid=world.startBuild(bid,yard,sx,sz,sim::World::Approach::None);
    const bool started=sid && world.unit(sid) && world.unit(sid)->type==yard &&
                       world.unit(sid)->underConstruction;
    std::printf("      armcs startBuild at (%.0f,%.0f): %s\n",bx,sz,started?"PASS":"FAIL");
    if (!started) ++failures;

    // Controlled terrain checks use the shipped yardmap on an otherwise empty
    // 64x64 grid, isolating water depth, the map border and occupied cells.
    const int x0=10,z0=10;
    const float tx=float(x0*16+yard->footX*8),tz=float(z0*16+yard->footZ*8);
    auto flat=[&](sim::World& w,int height) {
        constexpr int kSyntheticW=64,kSyntheticH=64;
        std::vector<uint8_t> h(size_t(kSyntheticW)*kSyntheticH,uint8_t(std::clamp(height,0,255)));
        w.setTerrain(h,kSyntheticW,kSyntheticH,map.seaLevel);
        w.buildNavClasses(reg);
    };
    sim::World deep,dry,land;
    flat(deep,map.seaLevel-40);
    flat(dry,map.seaLevel+10);
    if (!deep.canPlace(yard,tx,tz) || dry.canPlace(yard,tx,tz)) {
        std::printf("      water / dry land: FAIL\n"); ++failures;
    }
    // The one-cell map border: a building's footprint never touches row/column 0
    // (an off-map yard could never open for production).
    const float edgeX=float(yard->footX*8),edgeZ=float(yard->footZ*8);
    if (deep.canPlace(yard,tx,edgeZ) || deep.canPlace(yard,edgeX,tz) ||
        !deep.canPlace(yard,tx,edgeZ+16) || !deep.canPlace(yard,edgeX+16,tz)) {
        std::printf("      map border rows: FAIL\n"); ++failures;
    }
    std::vector<int> clearable;
    if (world.clearableForPlacement(yard,tx,edgeZ,clearable)) { std::printf("      border clearable: FAIL\n"); ++failures; }
    int ci=-1;
    for (size_t i=0;i<yard->yardMap.size();++i)
        if (ci<0 && yard->yardMap[i]=='C') ci=int(i);
    if (ci<0) { std::printf("      armsy yardmap has no C cell: FAIL\n"); ++failures; }
    else {
        deep.blockCells(x0+ci%yard->footX,z0+ci/yard->footX,1,1,true);
        const bool occupiedRejected=!deep.canPlace(yard,tx,tz);
        deep.blockCells(x0+ci%yard->footX,z0+ci/yard->footX,1,1,false);
        if (!occupiedRejected) { std::printf("      C occupancy: FAIL\n"); ++failures; }
    }
    if (const auto* lab=reg.find("armlab")) {
        flat(land,map.seaLevel+20);
        const float lx=float(24*16+lab->footX*8),lz=float(24*16+lab->footZ*8);
        if (!land.canPlace(lab,lx,lz) || deep.canPlace(lab,lx,lz)) {
            std::printf("      ordinary land factory rule: FAIL\n"); ++failures;
        }
    } else ++failures;
}
// Exercise the complete shipped production scripts of both sides' shipyards.
// Each ship must finish and make room for the next.
void shipyardProductionCases(const hpi::Vfs& vfs,const sim::TypeRegistry& reg) {
  for (const auto& [factory,side]:{std::pair{"armsy",0},std::pair{"corsy",1}}) {
    const auto* fort=reg.find(factory);
    if (!fort) { ++failures;continue; }
    for (const auto& name:reg.buildable(factory)) for (int angle:{-1,0,1}) {
        const auto* ship=reg.find(name);
        if (!ship) { ++failures;continue; }
        sim::World world;
        sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=hpi::findMap(vfs,kSeaMap);
        cfg.startEnergy=cfg.startMetal=1e9f;   // isolate placement, readiness and launch
        cfg.slots={{true,side,0,1.f,false,false}};
        sim::setupMatch(world,reg,cfg);
        float sx=0,sz=0;
        if (!findShipyardSite(world,fort,sx,sz)) { ++failures;continue; }
        // Both ends and the middle of the actual birth-heading range.
        const auto heading=sim::bamWrap(int(fort->orientation)+angle*int(fort->buildAngle)/2-(angle>0));
        const int id=world.spawn(fort,sx,sz,sim::radiansFromBam(heading),0);
        world.train(id,ship,2);
        for (int tick=0;tick<18000 && !world.unit(id)->buildQueue.empty();++tick) world.tick(1.f/30);
        int completed=0;
        for (const auto& u:world.units())
            completed+=u.alive() && u.type==ship && !u.underConstruction;
        const bool ok=completed==2 && world.unit(id)->buildQueue.empty();
        std::printf("      %s produces two %s (heading %d): %s\n",factory,name.c_str(),heading.v,ok?"PASS":"FAIL");
        if (!ok) ++failures;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: placement_test <retail-install>\n"); return 2; }
    std::printf("placement_test -- no unit starts on ground it cannot stand on\n");
    auto vfs = hpi::mountRetailRoot(argv[1]);
    {
        sim::TypeRegistry reg;
        sim::setupRegistry(reg,vfs);
        for (const auto& c : kCases) runCase(vfs, reg, c);
        stagedStressCase(vfs, reg, 0);
        stagedStressCase(vfs, reg, 1);
        shipyardPlacementCase(vfs,reg);
        shipyardProductionCases(vfs,reg);
    }
    std::printf(failures ? "placement_test: FAILURES (%d case(s))\n"
                         : "placement_test: all passed\n", failures);
    return failures ? 1 : 0;
}
