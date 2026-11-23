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
// The one this test was written for: the Monarch was the single spawn in setupMatch
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
// Needs a retail install, so it registers only with -DTAK_TEST_DATA.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "sim/footprint.h"
#include "client/renderframe.h"
#include "tnt/tnt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <string>
#include <vector>

using namespace tak;

namespace tak::sim {
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
    {"benchmark Low",         "Ulasem Arena", 8, 1, false},
    {"benchmark High",        "Ulasem Arena", 8, 3, false},
    {"benchmark Extra Absurd","Ulasem Arena", 8, 6, false},
    {"benchmark cramped",     "Inner Circle", 8, 4, false},
    {"stress Ulasem",         "Ulasem Arena", 8, 0, true},
    {"stress cramped",        "Inner Circle", 8, 0, true},
    // More slots than the map has start positions: the shortfall comes from a computed
    // ring too, and is just as unchecked as the benchmark's.
    {"overfilled starts",     "Two Castles",  8, 0, false},
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
    cfg.mapPath = std::string("maps/") + c.map + ".tnt";
    cfg.benchmark = c.benchmark;
    cfg.stressTest = c.stress;
    cfg.stressImmediate = true;                          // check the bodies, not a plan
    cfg.unitCap = 500;                                   // keep the stress fills quick
    for (int i = 0; i < c.players; ++i) {
        sim::MatchSlot s; s.used = true; s.faction = i % 5; s.team = i;
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
        if (u.type->id.find("king") != std::string::npos ||
            u.type->id.find("necro") != std::string::npos ||
            u.type->id.find("mage") != std::string::npos ||
            u.type->id.find("hunt") != std::string::npos ||
            u.type->id.find("sage") != std::string::npos)
            ++badMonarch;
        if (bad <= 3)
            std::printf("      %s at %.0f,%.0f does not fit its cell\n",
                        u.type->id.c_str(), u.x.toFloat(), u.z.toFloat());
    }
    std::printf("  %-24s %4d units, %d unplaceable%s\n", c.name, total, bad,
                badMonarch ? "  (INCLUDING A MONARCH)" : "");
    if (bad) ++failures;
}

// The staged stress fill (MatchConfig::stressImmediate=false, the game default) must
// place exactly the army the immediate fill places -- same player, type and position for
// every body -- only spread over ticks 1..kStressStageTicks, and must not turn the run
// into a benchmark (Monarchs stay mortal, no end tick).
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
        cfg.mapPath = "maps/Ulasem Arena.tnt";
        cfg.stressTest = true;
        cfg.stressImmediate = immediate;
        cfg.benchmark = bench;
        cfg.unitCap = 500;
        for (int i = 0; i < 8; ++i) {
            sim::MatchSlot s; s.used = true; s.faction = i % 5; s.team = i;
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
    // Both armies hold fire, so they stand where they were placed. A God's
    // mission scan on its melee slot reaches its 2500 px sight (4d8370), so
    // with fire at will a God of the immediate fill walks off after an enemy
    // the staged fill has not placed yet.
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

void seafortPlacementCase(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, bool crusades) {
    const std::string mapPath=hpi::findMap(vfs,"Varro Passage");
    const auto map=tnt::Map::load(vfs.read(mapPath),mapPath);
    const auto* seafort=reg.find("verasy");
    const auto* builder=reg.find("verliege");
    if (!seafort || !builder) { ++failures; return; }

    sim::World world;
    sim::MatchConfig cfg; cfg.vfs=&vfs; cfg.mapPath=mapPath;
    cfg.slots={{true,2,0,1.0f,false,false}};
    sim::setupMatch(world,reg,cfg);

    // The reported replay placed its fort at (1600,112). Its four ignored
    // leading rows hid an off-map yard, which could never open for production.
    for (float z : {112.f,128.f,144.f}) {
        if (world.canPlace(seafort,1600,z)) {
            std::printf("      off-map/border Sea Fort at z=%.0f accepted: FAIL\n",z);
            ++failures;
        }
        std::vector<int> clearable;
        if (world.clearableForPlacement(seafort,1600,z,clearable)) ++failures;
    }
    if (!world.canPlace(seafort,1600,160)) {
        std::printf("      first valid Sea Fort yard row: FAIL\n"); ++failures;
    }

    // Map-backed open-water Sea Fort site; center of the 6x18 yard at (1472,464).
    const float sx=1472, sz=464;
    const bool placeable=world.canPlace(seafort,sx,sz);
    std::printf("  Varro Passage Sea Fort (%s) open-water canPlace: %s\n",
                crusades?"Crusades":"Standard",placeable?"PASS":"FAIL");
    if (!placeable) { ++failures; return; }

    // Match the in-game Varro fixture: the Priestess stands 80px away, within its
    // 200px build distance, then starts the site through the ordinary API.
    const int bid=world.spawn(builder,1392,464,0,0);
    const int sid=world.startBuild(bid,seafort,sx,sz,sim::World::Approach::None);
    const bool started=sid && world.unit(sid) && world.unit(sid)->type==seafort &&
                       world.unit(sid)->underConstruction && builder->buildDist>=80;
    std::printf("      Veruna Priestess startBuild at (1392,464): %s\n",started?"PASS":"FAIL");
    if (!started) ++failures;

    // Controlled terrain checks use the shipped Sea Fort yardmap on an otherwise
    // empty 64x64 grid, isolating water depth and occupied cells.
    const int x0=10,z0=10;
    const float tx=float(x0*16+seafort->footX*8),tz=float(z0*16+seafort->footZ*8);
    auto flat=[&](sim::World& w,int height) {
        constexpr int kSyntheticW=64,kSyntheticH=64;
        std::vector<uint8_t> h(size_t(kSyntheticW)*kSyntheticH,uint8_t(height));
        w.setTerrain(h,kSyntheticW,kSyntheticH,map.seaLevel);
        w.buildNavClasses(reg);
    };
    sim::World deep,shallow,dry,land;
    flat(deep,map.seaLevel-30);
    flat(shallow,map.seaLevel-1);
    flat(dry,map.seaLevel);
    if (!deep.canPlace(seafort,tx,tz) || !shallow.canPlace(seafort,tx,tz) ||
        dry.canPlace(seafort,tx,tz)) { std::printf("      waterline depths: FAIL\n"); ++failures; }
    int ci=-1,di=-1;
    for (size_t i=0;i<seafort->yardMap.size();++i) {
        if (ci<0 && seafort->yardMap[i]=='C') ci=int(i);
        if (di<0 && seafort->yardMap[i]=='.') di=int(i);
    }
    if (ci<0 || di<0) { ++failures; }
    else {
        deep.blockCells(x0+ci%seafort->footX,z0+ci/seafort->footX,1,1,true);
        const bool occupiedRejected=!deep.canPlace(seafort,tx,tz);
        deep.blockCells(x0+ci%seafort->footX,z0+ci/seafort->footX,1,1,false);
        deep.blockCells(x0+di%seafort->footX,z0+di/seafort->footX,1,1,true);
        const bool dotIgnored=deep.canPlace(seafort,tx,tz);
        if (!occupiedRejected || !dotIgnored) { std::printf("      C occupancy / dot exclusion: FAIL\n"); ++failures; }
    }
    if (const auto* keep=reg.find("aracastl")) {
        flat(land,map.seaLevel+20);
        if (!land.canPlace(keep,float(24*16+keep->footX*8),
                           float(24*16+keep->footZ*8)) ||
            deep.canPlace(keep,float(24*16+keep->footX*8),
                          float(24*16+keep->footZ*8))) {
            std::printf("      ordinary Keep land rule: FAIL\n"); ++failures;
        }
    } else ++failures;
}
// Exercise the complete shipped production scripts at the nearest valid yard
// row to the reported fort. Each ship must finish and make room for the next.
void shipyardProductionCases(const hpi::Vfs& vfs,const sim::TypeRegistry& reg) {
  for (const auto& [factory,faction]:{std::pair{"verasy",2},std::pair{"crenavy",4}}) {
    const auto* fort=reg.find(factory);
    if (!fort) { ++failures;continue; }
    for (const auto& name:reg.buildable(factory)) for (int angle:{-1,0,1}) {
        const auto* ship=reg.find(name);
        if (!ship) { ++failures;continue; }
        sim::World world;
        sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=hpi::findMap(vfs,"Varro Passage");
        cfg.slots={{true,faction,0,1.f,false,false}};
        sim::setupMatch(world,reg,cfg);
        // Both ends and the middle of the actual birth-heading range. Creon's
        // yard does not counter-rotate its output like the Sea Fort does.
        const auto heading=sim::bamWrap(int(fort->orientation)+angle*int(fort->buildAngle)/2-(angle>0));
        const int id=world.spawn(fort,1600,160,sim::radiansFromBam(heading),0);
        world.train(id,ship,2);
        for (int tick=0;tick<18000 && !world.unit(id)->buildQueue.empty();++tick) {
            world.player(0).mana=100000; // isolate placement, readiness and launch
            world.tick(1.f/30);
        }
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
    for (bool crusades:{false,true}) {
        sim::TypeRegistry reg;
        sim::setupRegistry(reg, vfs, crusades);
        std::printf("balance: %s\n",crusades ? "Crusades" : "standard");
        for (const auto& c : kCases) runCase(vfs, reg, c);
        stagedStressCase(vfs, reg, 0);
        stagedStressCase(vfs, reg, 1);
        seafortPlacementCase(vfs,reg,crusades);
        shipyardProductionCases(vfs,reg);
        for (const char* id:{"aralode","tarlode","verlode","zonlode","crelode"}) {
            const auto* type=reg.find(id);
            if (!type) { ++failures;continue; }
            sim::World world;
            sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=hpi::findMap(vfs,"ulasem arena");
            cfg.slots={{true,0,0,1}};
            sim::setupMatch(world,reg,cfg);
            int accepted=0,unproductive=0;
            float px=0,pz=0,multiplier=0;
            for (const auto& [x,z]:world.manaSpots())
                for (int dx=-16;dx<=16;dx+=8) for (int dz=-16;dz<=16;dz+=8) {
                    if (!world.canPlace(type,x+dx,z+dz)) continue;
                    ++accepted;
                    multiplier=world.sacredIncomeMultiplier(
                        sim::footprintOrigin(x+dx,type->footX),sim::footprintOrigin(z+dz,type->footZ),
                        type->footX,type->footZ);
                    if (multiplier<=0) ++unproductive;
                    px=x+dx;pz=z+dz;
                }
            if (!accepted || unproductive) ++failures;
            world.player(0).mana=0;
            for (int i=0;i<31;++i) world.tick(1.f/30.f);
            PlayerR before;before.captureEconomy(world.player(0));
            const int lode=world.spawn(type,px,pz,0,0);
            world.unit(lode)->underConstruction=true;
            for (int i=0;i<31;++i) world.tick(1.f/30.f);
            PlayerR unfinished;unfinished.captureEconomy(world.player(0));
            if (std::abs(unfinished.income-before.income)>0.001 ||
                unfinished.storage!=before.storage) ++failures;
            world.unit(lode)->underConstruction=false;
            const double stored=world.player(0).mana;
            for (int i=0;i<60;++i) world.tick(1.f/30.f);
            PlayerR after;after.captureEconomy(world.player(0));
            const float expected=before.income+type->income*multiplier;
            if (std::abs(after.income-expected)>0.001 ||
                std::abs(after.mana-(stored+expected*2))>0.01 ||
                after.storage!=before.storage+type->storage) ++failures;
            std::printf("  %s: %d productive placements, %d zero-income placements; HUD income %.1f -> %.1f, capacity %.0f -> %.0f\n",
                        id,accepted,unproductive,before.income,after.income,before.storage,after.storage);
        }

    }
    std::printf(failures ? "placement_test: FAILURES (%d case(s))\n"
                         : "placement_test: all passed\n", failures);
    return failures ? 1 : 0;
}
