// unitdata_test -- the FBI loader against a REAL Total Annihilation install.
//
// Data-backed and therefore local-only: it takes an install path and is
// registered with ctest only when the build is configured with -DTA_TEST_DATA.
// CI never sees it, because the retail data cannot go on a runner.
//
// What it is for: the FBI key set was derived by surveying all 815 shipped unit
// files, and the values below were read out of those files by hand. Asserting
// them here turns "the parser compiles" into "the parser reads what Cavedog
// wrote" -- and catches the silent failure mode of a renamed key, where a unit
// loads fine with a field quietly left at its default.
//
//   usage: unitdata_test <retail-install-dir>

#include "hpi/hpi.h"
#include "tdf/sidedata.h"
#include "sim/sim.h"

#include <cstdio>
#include <string>

using ta::sim::TypeRegistry;
using ta::sim::UnitType;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) ++g_fail;
}
static void near(float got, float want, const std::string& what) {
    bool ok = std::abs(got - want) < 0.001f * std::max(1.0f, std::abs(want));
    std::printf("  %-70s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) {
        std::printf("      got %.4f, want %.4f\n", got, want);
        ++g_fail;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: unitdata_test <retail-install-dir>\n");
        return 2;
    }
    std::printf("unitdata_test\n");

    ta::hpi::Vfs vfs = ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/MOVEINFO.TDF");
    reg.loadDir(vfs, "units");

    // The Commander Pack ships 815 FBIs across base/CC/BT/patch, many of them the
    // same unit redefined by a later archive. A few hundred distinct types is the
    // right order; a handful would mean the mount or the extension filter broke.
    std::printf("  loaded %zu unit types\n", reg.types().size());
    check(reg.types().size() > 200, "several hundred unit types loaded");

    // --- ARM Commander: the unit every other assertion depends on -------------
    const UnitType* com = reg.find("armcom");
    check(com != nullptr, "armcom is in the registry");
    if (com) {
        near(com->buildCostMetal, 29854, "armcom BuildCostMetal");
        near(com->buildCostEnergy, 34125, "armcom BuildCostEnergy");
        near(com->maxHp, 3000, "armcom MaxDamage -> maxHp");
        near(com->buildTime, 95897, "armcom BuildTime");
        near(com->workerTime, 300, "armcom WorkerTime");
        near(com->sight, 290, "armcom SightDistance");
        near(com->radar, 400, "armcom RadarDistance");
        near(com->sonar, 400, "armcom SonarDistance");
        near(com->energyMake, 25, "armcom EnergyMake");
        near(com->metalMake, 1, "armcom MetalMake");
        near(com->buildDist, 60, "armcom BuildDistance");
        near(com->cloakCost, 200, "armcom CloakCost");
        near(com->cloakCostMove, 1000, "armcom CloakCostMoving");
        near(com->healTime, 27, "armcom HealTime");
        check(com->side == "ARM", "armcom Side=ARM");
        check(com->isBuilder, "armcom Builder=1");
        check(com->canDGun, "armcom candgun=1 (the disintegrator)");
        check(com->canReclaim, "armcom CanReclamate=1 -- the TA spelling is read");
        check(com->explodeAsName == "commander_blast", "armcom ExplodeAs names a weapon");
        check(com->selfDestructAsName == "commander_blast", "armcom SelfDestructAs likewise");
        // StandingMoveOrder=0 / StandingFireOrder=2: hold position, fire at will.
        check(com->defaultMove == 0, "armcom StandingMoveOrder=0 (hold position)");
        check(com->defaultFire == 2, "armcom StandingFireOrder=2 (fire at will)");
        check(com->canSetStance, "armcom mobilestandorders=1");
    }

    // --- The economy buildings, which is what the new fields exist for --------
    if (const UnitType* mex = reg.find("armmex")) {
        // 0.001 against a 3x3 patch of metal=127 is ~1.14 metal/s, which is what a
        // mex on a medium spot actually earns. If this key ever reads 0 the whole
        // metal economy silently stops without anything failing to load.
        near(mex->extractsMetal, 0.001f, "armmex ExtractsMetal");
        near(mex->energyUse, 3, "armmex EnergyUse");
        near(mex->buildCostMetal, 50, "armmex BuildCostMetal");
        near(mex->buildCostEnergy, 521, "armmex BuildCostEnergy");
        check(mex->onOffable, "armmex onoffable=1");
    } else {
        check(false, "armmex is in the registry");
    }
    if (const UnitType* win = reg.find("armwin")) {
        // A RATING, not a flag: output is this crossed with the map's wind.
        near(win->windGenerator, 30, "armwin WindGenerator=30 is read as a rating");
    } else {
        check(false, "armwin is in the registry");
    }
    if (const UnitType* tide = reg.find("armtide")) {
        check(tide->tidalGenerator > 0, "armtide TidalGenerator is set");
    } else {
        check(false, "armtide is in the registry");
    }

    // --- Both sides are present ----------------------------------------------
    int arm = 0, core = 0;
    for (const auto& [id, t] : reg.types()) {
        (void)id;
        if (t.side == "ARM") ++arm;
        else if (t.side == "CORE") ++core;
    }
    std::printf("  ARM %d, CORE %d\n", arm, core);
    check(arm > 50 && core > 50, "both ARM and CORE rosters loaded");

    // --- SIDEDATA: sides, commanders, build tree, HUD layout -----------------
    {
        ta::tdf::SideData sd = ta::tdf::SideData::load(vfs);
        check(sd.sides.size() == 2, "SIDEDATA declares two sides");
        const ta::tdf::Side* armS = sd.side("ARM");
        const ta::tdf::Side* corS = sd.side("CORE");
        check(armS && armS->commander == "armcom", "ARM's commander is ARMCOM");
        check(corS && corS->commander == "corcom", "CORE's commander is CORCOM");
        // CORE's unit-id prefix is COR, not its name -- so they must not be
        // conflated, and sideByPrefix has to exist for id-based lookups.
        check(corS && corS->namePrefix == "COR", "CORE's unit prefix is COR, not CORE");
        check(sd.sideByPrefix("COR") == corS, "lookup by unit-id prefix works");
        check(armS && armS->intGaf == "ARMINT", "ARM's interface GAF is named");

        // The panel rects are what a faithful two-resource HUD is drawn from, and
        // they are the reason the TDF parser had to stop reading values to end of
        // line: SIDEDATA puts all four keys of a rect on ONE line, so a
        // line-terminated value swallowed three of them and every rect came back
        // as x1 plus three zeros.
        const ta::tdf::PanelRect* mb = armS ? armS->panel("METALBAR") : nullptr;
        const ta::tdf::PanelRect* eb = armS ? armS->panel("ENERGYBAR") : nullptr;
        check(mb && mb->x1 == 218 && mb->y1 == 11 && mb->x2 == 339 && mb->y2 == 13,
              "ARM METALBAR rect reads all four coordinates");
        check(eb && eb->x1 == 471 && eb->y1 == 11 && eb->x2 == 592 && eb->y2 == 13,
              "ARM ENERGYBAR rect likewise");
        check(armS && armS->panels.size() > 25, "the whole panel table is read");

        // The build tree. Menu ORDER is the canbuildN numbering, so a naive walk
        // of a string-keyed map would sort canbuild10 between 1 and 2.
        auto it = sd.canBuild.find("armcom");
        check(it != sd.canBuild.end(), "ARMCOM has a build menu");
        if (it != sd.canBuild.end()) {
            const auto& m = it->second;
            check(m.size() == 12, "ARMCOM offers 12 buildings");
            check(m.size() > 6 && m[0] == "armsolar" && m[5] == "armmakr" &&
                  m[6] == "armlab",
                  "and in canbuild1..N order, not lexicographic");
        }
        check(sd.canBuild.size() > 20, "every builder's menu is read");

        // And the registry agrees with SIDEDATA about the commanders.
        for (const auto& c : {std::string("armcom"), std::string("corcom")})
            check(reg.find(c) != nullptr, "the registry has " + c);
    }

    // --- The Kingdoms keys are gone, and nothing quietly sets them ------------
    // A TA FBI has no buildcost/mogriumincome, so if any type comes back with a
    // Kingdoms-era field set, a stale parse is still running.
    int kingdomsy = 0;
    for (const auto& [id, t] : reg.types()) {
        (void)id;
        if (t.income != 0 || t.storage != 0) ++kingdomsy;
    }
    check(kingdomsy == 0, "no type carries a Kingdoms mogrium income/storage");

    std::printf(g_fail ? "unitdata_test: %d FAILURE(S)\n" : "unitdata_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
