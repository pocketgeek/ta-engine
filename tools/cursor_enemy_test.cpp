// Enemy-hover cursor over shipped unit types: Attack versus TooFar.
//
// Retail selector 0x4dd780 (tools/re/probe_cursor_enemy_hover.py) shows Attack for
// a MOBILE attacker over any enemy its active weapon can hurt, however far away --
// the unit walks over and attacks. Only an IMMOBILE attacker (a tower) needs the
// target inside its weapon's range and outside its minrange. The HUD used to
// apply the range test to every unit, so a selected archer showed the TooFar
// glyph over any enemy out of bowshot. The right-click attack order uses the same
// predicate, so it is issued exactly when this shows Attack.
//
// Needs a retail install, so it registers only with -DTAK_TEST_DATA.

#include "client/cursorrange.h"
#include "hpi/hpi.h"
#include "sim/sim.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace tak;

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
    const auto* archer = need("araarch");      // Archer, ballistic range 450
    const auto* sword = need("arasword");      // melee infantry
    const auto* mageArcher = need("arabow");   // weaponswitching: 3 arrows, range 550
    const auto* tortoise = need("cretort");    // mobile, minrange 64, range 805
    const auto* trebuchet = need("aratre");    // structure, minrange 500, range 2700
    const auto* galley = need("arawar");       // War Galley, noairweapon
    const auto* dragon = need("aradrag");      // armed flyer
    const auto* bastion = need("vertower");    // structure, range 580, minrange 180
    const auto* foe = need("tararch");         // ground target
    const auto* flyer = need("tarbeak");       // air target

    const auto can = [&](const sim::UnitType* a, int slot, int dist, const sim::UnitType* t,
                         uint8_t mode = kGround) {
        return client::cursorCanAttackEnemy(*a, slot, at(0), *t, at(dist), mode);
    };

    check(!archer->isStructure() && archer->hasPrimaryWeaponBlock, "archer is a mobile attacker");
    check(bastion->isStructure() && bastion->hasPrimaryWeaponBlock, "bastion is a structure attacker");

    // The reported bug: a mobile unit over an enemy beyond weapon range.
    check(can(archer, 0, 100, foe), "archer, enemy in range");
    check(can(archer, 0, 2000, foe), "archer, enemy beyond range (walks over)");
    check(can(sword, 0, 1500, foe), "melee, enemy far away");
    check(can(dragon, 0, 4000, foe), "flyer attacker, distant ground enemy");
    for (int slot = 0; slot < int(mageArcher->weapons.size()); ++slot)
        check(can(mageArcher, slot, 3000, foe),
              "weapon-switching archer, slot " + std::to_string(slot) + ", far enemy");
    // A mobile attacker's minrange does not gate the cursor either (it backs off).
    check(!tortoise->isStructure() && tortoise->weapons.front().minRange > 0,
          "tortoise is mobile with a minrange");
    check(can(tortoise, 0, 30, foe), "tortoise, enemy inside minrange");
    check(can(tortoise, 0, 3000, foe), "tortoise, enemy beyond range");

    // noairweapon against an airborne flyer, but not against one landed.
    check(galley->weapons.front().noAir, "galley weapon is noairweapon");
    check(!can(galley, 0, 300, flyer, kAirborne), "noairweapon vs airborne flyer");
    check(can(galley, 0, 300, foe), "noairweapon vs ground enemy");

    // toairweapon (no shipped user): only airborne targets.
    sim::UnitType antiAir = *archer;
    antiAir.weapons.front().toAir = true;
    check(!can(&antiAir, 0, 2000, foe), "toairweapon vs ground");
    check(can(&antiAir, 0, 2000, flyer, kAirborne), "toairweapon vs airborne");

    // notargetcategory lists the target.
    sim::UnitType picky = *archer;
    picky.noTargetCats = foe->categories;
    check(foe->categories.empty() || !can(&picky, 0, 100, foe), "notargetcategory target");

    // An immobile attacker still needs the target in range and outside minrange.
    check(can(bastion, 0, 400, foe), "bastion, enemy in range");
    check(!can(bastion, 0, 2000, foe), "bastion, enemy beyond range");
    check(!can(bastion, 0, 100, foe), "bastion, enemy inside minrange");
    check(trebuchet->isStructure(), "trebuchet is a structure");
    check(can(trebuchet, 0, 1000, foe), "trebuchet, enemy in range");
    check(!can(trebuchet, 0, 200, foe), "trebuchet, enemy inside minrange");
    check(!can(trebuchet, 0, 5000, foe), "trebuchet, enemy beyond range");

    // Every shipped pair whose damage is zero (category immunity) is TooFar,
    // mobile or not; at least one such pair must exist for this to mean anything.
    int immune = 0;
    for (const auto& [aid, a] : reg.types()) {
        if (!a.hasPrimaryWeaponBlock || a.weapons.empty() || a.weaponSwitching) continue;
        for (const auto& [tid, t] : reg.types()) {
            if (client::cursorDamageVs(a.weapons.front(), t) > 0.0f) continue;
            ++immune;
            check(!client::cursorCanAttackEnemy(a, 0, at(0), t, at(50), kGround),
                  aid + " vs immune " + tid);
        }
    }
    check(immune > 0, "some shipped attacker/target pair is damage-immune");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("PASS: enemy hover cursor matches retail for %d immune pairs and all cases\n",
                immune);
    return 0;
}
