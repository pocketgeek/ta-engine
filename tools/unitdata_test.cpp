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
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "ai/ai.h"

#include <cstdio>
#include <filesystem>
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
    // Is this a Commander Pack (base + Core Contingency + Battle Tactics) or a
    // plain base install? It decides several of the numbers below, because the
    // expansions REDEFINE them -- and because the engine now lets them, which it
    // did not always: with no timestamps in HPI v1 every collision is a tie, and
    // a tie used to keep the first-mounted archive, so the base `.hpi` beat
    // ccdata.ccx / btdata.ccx / rev31.gp3 on all 1082 paths where their content
    // differs. Asserting the base values unconditionally is what let that stand.
    const bool cc = std::filesystem::exists(std::filesystem::path(argv[1]) / "ccdata.ccx");
    std::printf("  install: %s\n", cc ? "Commander Pack (expansions present)" : "base only");
    // Go through setupRegistry rather than calling the loaders by hand: the
    // ORDER matters (weapons before units, or every unit loads unarmed) and a
    // test that reproduces the order itself would not catch getting it wrong.
    TypeRegistry reg;
    ta::sim::setupRegistry(reg, vfs);

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
        // Core Contingency moves and widens both bars: y 11->12, and the right
        // edge out by six pixels (METALBAR 339->345, ENERGYBAR 592->598).
        check(mb && mb->x1 == 218 && mb->y1 == (cc ? 12 : 11) && mb->x2 == (cc ? 345 : 339) &&
                  mb->y2 == (cc ? 14 : 13),
              "ARM METALBAR rect reads all four coordinates");
        check(eb && eb->x1 == 471 && eb->y1 == (cc ? 12 : 11) && eb->x2 == (cc ? 598 : 592) &&
                  eb->y2 == (cc ? 14 : 13),
              "ARM ENERGYBAR rect likewise");
        check(armS && armS->panels.size() > 25, "the whole panel table is read");

        // The build tree. Menu ORDER is the canbuildN numbering, so a naive walk
        // of a string-keyed map would sort canbuild10 between 1 and 2.
        auto it = sd.canBuild.find("armcom");
        check(it != sd.canBuild.end(), "ARMCOM has a build menu");
        if (it != sd.canBuild.end()) {
            const auto& m = it->second;
            // The base Commander's menu is 12 entries; Core Contingency extends
            // every builder's menu (sidedata.tdf goes from 283 canbuild lines to
            // 474), which takes the Commander from 12 entries to 19 -- the
            // extra seven being its naval and underwater construction. The
            // first six and the Kbot Lab keep their places either way.
            check(m.size() == (cc ? 19u : 12u), "ARMCOM's build menu is the expected length");
            check(m.size() > 6 && m[0] == "armsolar" && m[5] == "armmakr" &&
                  m[6] == "armlab",
                  "and in canbuild1..N order, not lexicographic");
        }
        check(sd.canBuild.size() > 20, "every builder's menu is read");

        // And the registry agrees with SIDEDATA about the commanders.
        for (const auto& c : {std::string("armcom"), std::string("corcom")})
            check(reg.find(c) != nullptr, "the registry has " + c);
    }

    // --- Weapons: the shared tables, resolved by name ------------------------
    // TA keeps 686 weapon definitions in weapons/**.tdf and gamedata/WEAPONS.TDF
    // and a unit's FBI only NAMES the ones it carries. Kingdoms inlined a
    // [WEAPONn] section per FBI, so before this was wired every one of the 278
    // TA unit types loaded unarmed -- and nothing failed, the game was just
    // silently pacifist. Hence the blunt count assertion first.
    {
        int armed = 0;
        for (const auto& [id, t] : reg.types()) { (void)id; if (!t.weapons.empty()) ++armed; }
        std::printf("  armed types: %d of %zu\n", armed, reg.types().size());
        check(armed > 100, "a large fraction of unit types are armed");

        const ta::sim::Weapon* laser = reg.weapon("ARMCOMLASER");
        check(laser != nullptr, "the weapon table resolves ARMCOMLASER by name");
        check(reg.weapon("armcomlaser") != nullptr, "...case-insensitively");
        if (laser) {
            near(laser->damage, 60, "ARMCOMLASER damage");
            near(laser->range, 200, "ARMCOMLASER range");
            near(laser->reload, 0.85f, "ARMCOMLASER reloadtime");
            near(laser->aoe, 16, "ARMCOMLASER areaofeffect");
        }
        if (com) {
            check(com->weapons.size() == 2, "armcom carries two weapons");
            // Weapon1 = the laser, Weapon3 = ARM_DISINTEGRATOR, the D-gun. Its
            // 5500 damage is the single most recognisable number in the game --
            // until Core Contingency raised it to 30000.
            if (com->weapons.size() == 2) {
                near(com->weapons[0].damage, 60, "armcom W1 is the J7 Laser");
                // The D-gun: 5500 damage in the base game, raised to 30000 by
                // Core Contingency. (The engine served the base number even on a
                // Commander Pack install until the HPI tie-break was fixed.)
                near(com->weapons[1].damage, cc ? 30000 : 5500,
                     "armcom W3 is the Disintegrator");
                near(com->weapons[1].range, 240, "...at D-gun range");
            }
        }
        if (const UnitType* pw = reg.find("armpw")) {
            check(pw->weapons.size() == 1, "the Peewee has one weapon");
            if (!pw->weapons.empty()) near(pw->weapons[0].damage, 8, "Peewee E.M.G. damage");
        } else {
            check(false, "armpw is in the registry");
        }
        // A weapon the tables do not define must leave the slot empty rather than
        // inventing a zero-damage one that would make the unit look armed.
        check(reg.weapon("NO_SUCH_WEAPON_XYZ") == nullptr, "an unknown weapon resolves to nothing");
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

    // --- Negative EnergyUse is PRODUCTION -------------------------------------
    // Both sides' Solar Collector states its whole output as "EnergyUse=-20;
    // EnergyMake=0". Nothing downstream should ever see a negative use: read as a
    // drain, a negative total made the entire base's upkeep free; ignored, solar
    // panels produced nothing and neither side had a first energy building that
    // worked. This is the one place the normalisation can be checked against the
    // shipped data rather than a synthetic type.
    for (const char* id : {"armsolar", "corsolar"}) {
        const auto* t = reg.find(id);
        check(t != nullptr, std::string(id) + " is in the registry");
        if (!t) continue;
        check(t->energyUse == 0,
              std::string(id) + " exposes no negative energy use");
        check(t->energyMake >= 20.0f,
              std::string(id) + " earns its 20/sec through energyMake");
    }
    int negUse = 0;
    for (const auto& [id, t] : reg.types()) {
        (void)id;
        if (t.energyUse < 0 || t.metalUse < 0) ++negUse;
    }
    check(negUse == 0, "no type exposes a negative standing use at all");

    // --- the AI's factory appetite -------------------------------------------
    // The economy ladder is stated in "one factory's worth of income" rather than
    // a raw number, and that quantity is DERIVED from this install. It replaced a
    // hardcoded 20, which is a Kingdoms mana figure: TA's metal runs an order of
    // magnitude smaller, and Coast To Coast has ten metal patches for BOTH
    // players, so 20/sec is not reachable on it at all -- the AI's switch to
    // production ended up gated on running out of economy to build rather than on
    // earning enough to run a factory. So the derived figure has to land in the
    // band the early game actually plays in, or the same failure returns wearing
    // a different constant.
    {
        ta::ai::Profile prof;
        ta::ai::Controller ai(0, reg, prof, 1, ta::ai::Difficulty::Normal, {});
        const float draw = ai.factoryAppetite();
        std::printf("      (measured factory appetite: %.2f metal/sec)\n", double(draw));
        check(draw > 2.0f && draw < 10.0f,
              "the factory appetite lands in TA's tier-1 band, not Kingdoms' magnitudes");

        // ...and it should agree with the labs the opening actually builds from.
        // A Kbot Lab's is buildCostMetal * workerTime / buildTime, averaged over
        // its menu; the derived median must sit near that, not near an advanced
        // shipyard's (16/sec) or an aircraft plant's (1.4/sec).
        const auto* lab = reg.find("armlab");
        check(lab != nullptr, "armlab resolves");
        if (lab && lab->workerTime > 0) {
            double sum = 0; int n = 0;
            for (const auto& id : reg.buildable("armlab")) {
                const auto* m = reg.find(id);
                if (!m || m->buildTime <= 0 || m->buildCostMetal <= 0 || m->isStructure())
                    continue;
                sum += double(m->buildCostMetal) * lab->workerTime / m->buildTime;
                ++n;
            }
            const double labDraw = n ? sum / n : 0;
            std::printf("      (armlab draws %.2f metal/sec over %d menu entries)\n",
                        labDraw, n);
            check(n > 0 && draw > labDraw * 0.5 && draw < labDraw * 2.0,
                  "and is within a factor of two of a Kbot Lab's own draw");
        }
    }

    // --- commandfire: the weapons that only fire when ordered -----------------
    // WEAPONS.TDF marks exactly eight: both Disintegrators (the Commander's
    // D-gun), the four bombs, the nuclear missile and CRBLMSSL. Without the flag
    // a bomber auto-drops on whatever it drifts over, a silo launches at the first
    // thing it sees, and the Commander's acquisition radius becomes D-gun range.
    {
        const auto* dis = reg.weapon("ARM_DISINTEGRATOR");
        check(dis != nullptr, "ARM_DISINTEGRATOR is in the weapon table");
        check(dis && dis->commandFire, "and is marked command-fire");
        const auto* laser = reg.weapon("ARMCOMLASER");
        check(laser != nullptr, "ARMCOMLASER is too");
        check(laser && !laser->commandFire,
              "but the Commander's ordinary laser is NOT command-fire");

        const auto* com = reg.find("armcom");
        check(com != nullptr, "armcom resolves");
        if (com) {
            const int slot = com->commandFireSlot();
            check(slot >= 0, "the Commander has a command-fire weapon for BLAST to arm");
            check(slot >= 0 && com->slotIsCommandFire(slot),
                  "and that slot reports itself as command-fire");
            // The D-gun outranges the laser, so the two ranges must differ -- this
            // is what makes counting it in the auto radius a real mistake.
            check(com->maxAutoRange() < com->maxRange(),
                  "the Commander's unbidden reach is SHORTER than its full reach");
            check(slot != 0, "and it is not the default weapon, so it is never armed by accident");
        }
    }

    std::printf(g_fail ? "unitdata_test: %d FAILURE(S)\n" : "unitdata_test: all passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
