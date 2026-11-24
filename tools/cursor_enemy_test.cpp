// Enemy-hover cursor over shipped unit types: Attack versus TooFar.
//
// Retail selector 0x4dd780 (tools/re/probe_cursor_enemy_hover.py) shows Attack for
// a MOBILE attacker over any enemy its active weapon can hurt, however far away --
// the unit walks over and attacks. Only an IMMOBILE attacker (a tower) needs the
// target inside its weapon's range and outside its minrange. The HUD used to
// apply the range test to every unit, so a selected unit showed the TooFar
// glyph over any enemy out of weapon range. The right-click attack order uses the same
// predicate, so it is issued exactly when this shows Attack.
//
// Needs a retail install, so it registers only with -DTA_TEST_DATA.

#include "client/cursorrange.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ta;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; }
}

std::array<int32_t, 3> at(int px) { return {int32_t(px) * 65536, 0, 0}; }

constexpr uint8_t kGround = 1, kAirborne = 2;
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cursor_enemy_test <retail-data>\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");
    reg.internCategories();
    const auto need = [&](const char* name) {
        const sim::UnitType* t = reg.find(name);
        if (!t) { std::fprintf(stderr, "FAIL: shipped type %s missing\n", name); std::exit(1); }
        return t;
    };
    const auto* hammer = need("armham");       // Hammer, mobile, ballistic plasma, range 320
    const auto* peewee = need("armpw");        // Peewee, mobile, E.M.G., range 180
    const auto* brawler = need("armbrawl");    // Brawler, armed gunship
    const auto* phalanx = need("armyork");     // Phalanx, mobile, toairweapon flak
    const auto* flakker = need("armflak");     // Flakker, structure, toairweapon, range 700
    const auto* guardian = need("armguard");   // Guardian, structure, ballistic, range 1250
    const auto* llt = need("armllt");          // L.L.T., structure, range 300
    const auto* foe = need("corak");           // ground target
    const auto* flyer = need("corveng");       // air target

    const auto can = [&](const sim::UnitType* a, int slot, int dist, const sim::UnitType* t,
                         uint8_t mode = kGround) {
        return client::cursorCanAttackEnemy(*a, slot, at(0), *t, at(dist), mode);
    };

    check(!hammer->isStructure() && hammer->hasPrimaryWeaponBlock, "hammer is a mobile attacker");
    check(llt->isStructure() && llt->hasPrimaryWeaponBlock, "L.L.T. is a structure attacker");

    // The reported bug: a mobile unit over an enemy beyond weapon range.
    check(can(hammer, 0, 100, foe), "hammer, enemy in range");
    check(can(hammer, 0, 2000, foe), "hammer, enemy beyond range (walks over)");
    check(can(peewee, 0, 1500, foe), "peewee, enemy far away");
    check(can(brawler, 0, 4000, foe), "flyer attacker, distant ground enemy");

    // toairweapon: only airborne targets, mobile or not.
    check(phalanx->weapons.front().toAir, "phalanx weapon is toairweapon");
    check(!can(phalanx, 0, 300, foe), "toairweapon vs ground");
    check(can(phalanx, 0, 2000, flyer, kAirborne), "mobile toairweapon vs distant airborne");
    check(!can(flakker, 0, 300, foe), "toairweapon structure vs ground");
    check(can(flakker, 0, 300, flyer, kAirborne), "toairweapon structure vs airborne in range");
    check(!can(flakker, 0, 2000, flyer, kAirborne), "toairweapon structure vs airborne beyond range");

    // notargetcategory lists the target.
    sim::UnitType picky = *hammer;
    picky.noTargetCats = foe->categories;
    check(foe->categories.empty() || !can(&picky, 0, 100, foe), "notargetcategory target");

    // An immobile attacker still needs the target in range. (No TA weapon
    // has a minrange or noairweapon, so those gates are not exercised here.)
    check(can(llt, 0, 250, foe), "L.L.T., enemy in range");
    check(!can(llt, 0, 2000, foe), "L.L.T., enemy beyond range");
    check(guardian->isStructure(), "guardian is a structure");
    check(can(guardian, 0, 1000, foe), "guardian, enemy in range");
    check(!can(guardian, 0, 5000, foe), "guardian, enemy beyond range");

    // A pair whose damage is zero is TooFar, mobile or not. TA ships no such
    // pair (every [DAMAGE] table but NOWEAPON's is positive), so a Hammer and
    // an L.L.T. with zeroed damage stand in; the shipped loop below checks any
    // pair a mod might add.
    for (const sim::UnitType* base : {hammer, llt}) {
        sim::UnitType harmless = *base;
        auto& w = harmless.weapons.front();
        w.damage = 0; w.dmgVs.clear(); w.dmgVsIds.clear();
        check(!can(&harmless, 0, 50, foe), base->id + " with zero damage vs ground enemy");
    }
    int immune = 0;
    for (const auto& [aid, a] : reg.types()) {
        if (!a.hasPrimaryWeaponBlock || a.weapons.empty()) continue;
        for (const auto& [tid, t] : reg.types()) {
            if (client::cursorDamageVs(a.weapons.front(), t) > 0.0f) continue;
            ++immune;
            check(!client::cursorCanAttackEnemy(a, 0, at(0), t, at(50), kGround),
                  aid + " vs immune " + tid);
        }
    }

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("PASS: enemy hover cursor matches retail for %d immune pairs and all cases\n",
                immune);
    return 0;
}
