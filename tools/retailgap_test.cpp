// retailgap_test: verifies the retail-parity sim fixes that have no other harness.
//
//   retailgap_test <retail-install-dir>
//
// Checks, against the real shipped data:
//   1. [EXPLODEAS] parses, and a dying unit detonates it where it stands (splashing
//      nearby enemies) -- the Kamikaze Rat's whole purpose.
//   2. totalallowed parses and caps live units of that type per player (dragons).
//   3. A data-only mission (no .cob) loads, and its placed units' InitialMission=
//      order queues are applied.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace tak;

namespace {
int failures = 0;
const char* kMap = "maps/Inner Circle.tnt";   // any shipped map; we only need terrain

// Nudge a spawn point onto ground the unit actually fits on. Fixtures used to
// pick round numbers and rely on the sim's "unstick" pass to walk a unit off
// whatever rock it landed on; that pass is gone (retail has none), so a fixture
// that spawns on a cliff now simply has a unit that cannot move.
static void legalSpot(const tak::sim::World& w, const tak::sim::UnitType* t,
                      float& x, float& z) {
    const tak::sim::NavGrid& g = w.navFor(t);
    if (g.empty() || !t) return;
    const int foot = std::max(1, std::max(t->footX, t->footZ));
    const int cx = int(x) / 16, cz = int(z) / 16;
    if (g.fits(cx, cz, foot)) return;
    for (int r = 1; r <= 24; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx)
                if (std::abs(dx) == r || std::abs(dz) == r)
                    if (g.fits(cx + dx, cz + dz, foot)) {
                        x = float(cx + dx) * 16 + 8;
                        z = float(cz + dz) * 16 + 8;
                        return;
                    }
}
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}
float detmathLen(float dx, float dz) { return std::sqrt(dx * dx + dz * dz); }
void tick(sim::World& w, float seconds) {
    const float dt = 1.0f / 30.0f;
    for (int i = 0; i < int(seconds / dt); ++i) w.tick(dt);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: retailgap_test <retail-install-dir>\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");

    // ---- 1. [EXPLODEAS] ----------------------------------------------------
    std::printf("[EXPLODEAS death blast]\n");
    const sim::UnitType* rat = reg.find("tarkam");        // Kamikaze Rat
    const sim::UnitType* victim = reg.find("arasword");    // a plain Aramon swordsman
    if (!rat || !victim) { std::printf("  (missing unit defs; skipped)\n"); }
    else {
        check(rat->hasExplodeAs, "tarkam parses an EXPLODEAS weapon");
        check(rat->explodeAs.aoe > 100.0f, "its blast has a real radius",
              "aoe=" + std::to_string(int(rat->explodeAs.aoe)));
        check(rat->explodeAs.damage > 1000.0f, "and real damage",
              "dmg=" + std::to_string(int(rat->explodeAs.damage)));

        sim::World w;
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = kMap;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        cfg.slots[0].team = 0; cfg.slots[1].team = 1;
        sim::setupMatch(w, reg, cfg);

        // A rat (player 0) with two enemy swordsmen (player 1) standing next to it.
        int ratId = w.spawn(rat, 500, 500, 0, 0);
        int aId = w.spawn(victim, 520, 500, 0, 1);
        int bId = w.spawn(victim, 500, 530, 0, 1);
        int farId = w.spawn(victim, 500, 1200, 0, 1);   // well outside the blast
        float aHp0 = w.unit(aId)->hp, farHp0 = w.unit(farId)->hp;
        check(aHp0 > 0 && farHp0 > 0, "victims spawned alive");

        w.unit(ratId)->hp = 0;    // the rat dies -> its EXPLODEAS should fire
        tick(w, 0.2f);

        const sim::Unit* a = w.unit(aId);
        const sim::Unit* b = w.unit(bId);
        const sim::Unit* far = w.unit(farId);
        check(a && (!a->alive() || a->hp < aHp0), "adjacent enemy took the blast");
        check(b && (!b->alive() || b->hp < aHp0), "second adjacent enemy took the blast");
        check(far && far->alive() && far->hp >= farHp0, "distant enemy untouched");
    }

    // ---- 1b. weapon-class parse ------------------------------------------
    std::printf("[weapon class parse]\n");
    {
        struct C { const char* unit; int slot; sim::Weapon::Kind kind; const char* label; };
        const C cases[] = {
            {"arabow",   1, sim::Weapon::Kind::Guided,    "arabow W2 Tracking Arrow = Guided"},
            {"aradrag",  2, sim::Weapon::Kind::Remote,    "aradrag W3 Earthquake = Remote Effect"},
            {"tarwitch", 0, sim::Weapon::Kind::Wandering, "tarwitch W1 Tornado = Wandering"},
        };
        for (const C& c : cases) {
            const sim::UnitType* t = reg.find(c.unit);
            bool ok = t && int(t->weapons.size()) > c.slot && t->weapons[size_t(c.slot)].kind == c.kind;
            check(ok, c.label);
        }
        if (const sim::UnitType* t = reg.find("tarwitch"); t && !t->weapons.empty()) {
            const sim::Weapon& w = t->weapons[0];
            check(w.duration > 8.0f, "tornado duration parsed", std::to_string(w.duration));
            check(w.buildUp > 2.0f && w.decay > 2.0f, "buildup/decay parsed");
            check(w.variationTime > 0 && w.maxVariation > 0, "wander variation parsed");
        }
        if (const sim::UnitType* t = reg.find("tarmind"); t && !t->weapons.empty()) {
            check(t->weapons[0].mindControl, "tarmind W1 flagged mindControl");
            check(t->weapons[0].unitsOnly, "tarmind W1 unitsonly");
            // The DAMAGE table is the eligibility filter.
            const sim::UnitType* monarch = reg.find("araking");
            const sim::UnitType* sword = reg.find("arasword");
            if (monarch && sword) {
                check(t->weapons[0].damageVs(monarch) <= 0.0f, "monarch is mind-control IMMUNE (damage 0)");
                check(t->weapons[0].damageVs(sword) > 0.0f, "a swordsman is convertible (damage > 0)");
            }
        }
        if (const sim::UnitType* t = reg.find("arabow"); t && t->weapons.size() > 1)
            check(t->weapons[1].turnRate > 3.0f, "guided turnrate in radians/s",
                  std::to_string(t->weapons[1].turnRate));
    }

    // ---- 2. totalallowed ---------------------------------------------------
    std::printf("[totalallowed unique cap]\n");
    const sim::UnitType* drag = reg.find("aradrag");
    if (!drag) { std::printf("  (no aradrag; skipped)\n"); }
    else {
        check(drag->totalAllowed == 1, "aradrag parses totalallowed=1",
              "got " + std::to_string(drag->totalAllowed));
        sim::World w;
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = kMap;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        cfg.slots[0].team = 0; cfg.slots[1].team = 1;
        sim::setupMatch(w, reg, cfg);
        check(!w.atTypeCap(0, drag), "no dragon yet -> not capped");
        int d1 = w.spawn(drag, 400, 400, 0, 0);
        check(w.atTypeCap(0, drag), "one live dragon -> player 0 is capped");
        check(!w.atTypeCap(1, drag), "the cap is PER PLAYER (player 1 free)");
        if (auto* d = w.unit(d1)) d->hp = 0;   // it dies
        tick(w, 0.2f);
        check(!w.atTypeCap(0, drag), "dragon died -> the slot frees again");
        const sim::UnitType* plain = reg.find("arasword");
        if (plain) check(!w.atTypeCap(0, plain), "an uncapped type is never capped");
    }

    // ---- 2b. guided homing --------------------------------------------------
    std::printf("[guided homing]\n");
    {
        // tarfire (Fire Demon) has a GUIDED primary: Fire Ball of Gloom, range 900,
        // velocity 250 (slow -- long flight), turnrate 180, reload 3.2s. The slow
        // shot + long reload make the dodge unambiguous: one shot is in the air, and
        // only homing can bring it back onto a target that jumps aside mid-flight.
        const sim::UnitType* demon = reg.find("tarfire");
        const sim::UnitType* prey = reg.find("arasword");
        if (!demon || !prey || demon->weapons.empty()) std::printf("  (missing defs; skipped)\n");
        else {
            check(demon->weapons[0].kind == sim::Weapon::Kind::Guided &&
                  demon->weapons[0].turnRate > 0, "Fire Ball of Gloom is guided");
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int shooter = w.spawn(demon, 1000, 1000, 0, 0);
            int mover = w.spawn(prey, 1000, 1600, 0, 1);   // 600px away, in range
            if (auto* su = w.unit(shooter)) su->stance = 0;
            w.attack(shooter, mover, false);
            // Tick until a shot is in the air.
            int guard = 0;
            while (w.projectiles().empty() && guard++ < 400) w.tick(1.0f / 30.0f);
            check(!w.projectiles().empty(), "the demon launched a guided shot");
            if (!w.projectiles().empty()) {
                // Jump the target 300px sideways: a straight shot now misses by far
                // more than the hit radius.
                if (auto* m = w.unit(mover)) { m->x = 1300; m->orders.clear(); }
                float hp0 = w.unit(mover)->hp;
                for (int i = 0; i < 90; ++i) w.tick(1.0f / 30.0f);   // 3s < reload
                const sim::Unit* m = w.unit(mover);
                bool hurt = m && (!m->alive() || m->hp < hp0);
                check(hurt, "the shot curved onto the dodging target",
                      hurt ? "" : "untouched -- homing did not engage");
            }
        }
    }

    // ---- 2c. remote effect / wandering / mind control ------------------------
    std::printf("[remote effect + wandering + mind control]\n");
    {
        const sim::UnitType* prey = reg.find("arasword");
        auto freshWorld = [&](sim::World& w) {
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
        };

        // Remote Effect: the Acolyte's Earthquake lands at the AIMED GROUND POINT
        // after builduptime -- stepping aside must not dodge it (aoe 200).
        const sim::UnitType* acolyte = reg.find("arapries");
        if (acolyte && prey) {
            int quake = -1;
            for (size_t i = 0; i < acolyte->weapons.size(); ++i)
                if (acolyte->weapons[i].kind == sim::Weapon::Kind::Remote) { quake = int(i); break; }
            check(quake >= 0, "arapries has a Remote Effect weapon");
            if (quake >= 0) {
                sim::World w; freshWorld(w);
                int caster = w.spawn(acolyte, 1000, 1000, 0, 0);
                int victim = w.spawn(prey, 1000, 1150, 0, 1);
                if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
                w.attack(caster, victim, false);
                float hp0 = w.unit(victim)->hp;
                for (int i = 0; i < 600; ++i) {
                    w.tick(1.0f / 30.0f);
                    if (const sim::Unit* v = w.unit(victim); v && (!v->alive() || v->hp < hp0)) break;
                }
                const sim::Unit* v = w.unit(victim);
                check(v && (!v->alive() || v->hp < hp0), "the remote effect landed on its target");
            }
        }

        // Remote Effect SUBCLASSES: the cadence differs per subtype.
        // arapries: W1 Earthquake (pulses every shakeduration=1s through its
        // buildup 3 + decay 3), W2 Hail Shower (rains particlespersecond=5 times a
        // second for duration=2.5), W3 Turn To Stone.
        if (acolyte && prey && acolyte->weapons.size() >= 3) {
            check(acolyte->weapons[0].remote == sim::Weapon::RemoteKind::Earthquake,
                  "arapries W1 is the Earthquake subclass");
            check(acolyte->weapons[1].remote == sim::Weapon::RemoteKind::Hailstorm,
                  "arapries W2 is the Hailstorm subclass");
            // THE BUG FIX: "Hail Shower" used to trip a name-substring freeze
            // heuristic, and our freeze is an instant statue death -- so a 70-damage
            // shower silently annihilated everything in a 200-wide radius.
            check(acolyte->weapons[1].status == sim::Weapon::Status::None,
                  "Hail Shower is NOT a freeze weapon (it used to instant-kill)");
            check(acolyte->weapons[2].status == sim::Weapon::Status::Stoned,
                  "Turn To Stone still petrifies (subtype=turntostone)");

            // Drive each and count how many distinct ticks dealt damage: a plain
            // one-shot would show exactly 1, these must pulse.
            auto pulseCount = [&](int slot, float seconds) {
                sim::World w; freshWorld(w);
                int caster = w.spawn(acolyte, 1000, 1000, 0, 0);
                int victim = w.spawn(prey, 1000, 1040, 0, 1);   // inside aoe/2 = 100
                if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
                w.setWeapon(caster, slot);
                w.attack(caster, victim, false);
                int pulses = 0; float last = w.unit(victim)->hp; bool died = false;
                for (int i = 0; i < int(seconds * 30); ++i) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* v = w.unit(victim);
                    if (!v || !v->alive()) { died = true; break; }
                    if (v->hp < last - 0.01f) { ++pulses; last = v->hp; }
                }
                return std::pair<int, bool>{pulses, died};
            };
            // shotart: this Earthquake is DELIVERED -- a visible shot flies to the
            // aim point first and only then does the 3s channel start, so the first
            // damage lands later than a conjured-in-place spell would.
            check(!acolyte->weapons[0].shotArt.empty(), "arapries W1 carries shotart");
            {
                sim::World w; freshWorld(w);
                int caster = w.spawn(acolyte, 1000, 1000, 0, 0);
                int victim = w.spawn(prey, 1000, 1200, 0, 1);   // 200px: a real flight
                if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
                w.setWeapon(caster, 0);
                w.attack(caster, victim, false);
                bool sawShot = false;
                for (int i = 0; i < 240; ++i) {
                    w.tick(1.0f / 30.0f);
                    if (!w.projectiles().empty()) { sawShot = true; break; }
                }
                check(sawShot, "it launches a visible delivery shot");
            }
            auto [quakePulses, quakeDied] = pulseCount(0, 8.0f);
            check(quakePulses > 1 || quakeDied, "the Earthquake PULSES (not one tap)",
                  std::to_string(quakePulses) + " damage ticks");
            auto [hailPulses, hailDied] = pulseCount(1, 8.0f);
            check(hailPulses > 3, "the Hail Shower RAINS many small hits",
                  std::to_string(hailPulses) + " damage ticks");
            check(!hailDied, "and its victim survives it (70 dmg x ~13 < 2500 HP)");
        }

        // Wandering: the Tornado is a MOVING HAZARD, not a targeted strike -- it is
        // born in front of the caster, drifts along the launch heading and weaves
        // hard sideways (maxvariation is px/tick and dwarfs the drift), grinding
        // whatever it touches every tick. So seed the area it will cross and check
        // it chews through units over time rather than dealing one tap.
        const sim::UnitType* witch = reg.find("tarwitch");
        if (witch && prey && !witch->weapons.empty() &&
            witch->weapons[0].kind == sim::Weapon::Kind::Wandering) {
            sim::World w; freshWorld(w);
            float wx = 1000, wz = 1000;
            legalSpot(w, witch, wx, wz);
            int caster = w.spawn(witch, wx, wz, 0, 0);
            if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
            // A dense field across the whole region a 9-second tornado can reach
            // (it drifts ~400px forward and wanders hundreds of px sideways), so the
            // check does not depend on the exact RNG path.
            std::vector<int> crowd;
            for (int gz = 0; gz < 11; ++gz)
                for (int gx = 0; gx < 11; ++gx)
                    crowd.push_back(w.spawn(prey, 900.0f + gx * 45.0f,
                                            1100.0f + gz * 45.0f, 0, 1));
            float total0 = 0;
            for (int id : crowd) if (auto* u = w.unit(id)) total0 += u->hp;
            w.attack(caster, crowd[12], false);
            int ticksWithDamage = 0;
            float last = total0;
            for (int i = 0; i < 600; ++i) {
                w.tick(1.0f / 30.0f);
                float now = 0;
                for (int id : crowd) if (const sim::Unit* u = w.unit(id); u && u->alive()) now += u->hp;
                if (now < last - 0.01f) ++ticksWithDamage;
                last = now;
            }
            check(last < total0, "the tornado chewed through the crowd it crossed",
                  "lost " + std::to_string(int(total0 - last)) + " HP");
            check(ticksWithDamage > 5, "and it GRINDS continuously (many damage ticks)",
                  std::to_string(ticksWithDamage) + " ticks dealt damage");
        }

        // Mind control: the Mind Mage converts an eligible enemy; a Monarch is immune.
        const sim::UnitType* mage = reg.find("tarmind");
        if (mage && prey && !mage->weapons.empty()) {
            sim::World w; freshWorld(w);
            int caster = w.spawn(mage, 1000, 1000, 0, 0);
            int victim = w.spawn(prey, 1000, 1200, 0, 1);
            if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
            w.attack(caster, victim, false);
            bool converted = false;
            for (int i = 0; i < 900; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* v = w.unit(victim);
                if (v && v->alive() && v->player == 0) { converted = true; break; }
            }
            check(converted, "the Mind Mage CONVERTED an enemy swordsman to its side");
        }

        // A Monarch must be IMMUNE (its DAMAGE category is zeroed) -- and must not
        // be silently damaged by the 1-point nominal damage either.
        const sim::UnitType* king = reg.find("araking");
        if (mage && king) {
            sim::World w; freshWorld(w);
            int caster = w.spawn(mage, 1000, 1000, 0, 0);
            int royal = w.spawn(king, 1000, 1200, 0, 1);
            if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
            w.attack(caster, royal, false);
            bool stolen = false;
            for (int i = 0; i < 900; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* v = w.unit(royal);
                if (v && v->alive() && v->player == 0) { stolen = true; break; }
            }
            check(!stolen, "a Monarch resists mind control (commander gate)");
        }

        // Area Mind Control (Remote Effect + mindcontrol, aoe 250) should convert
        // EVERY eligible enemy in the radius, not just one.
        if (mage && prey && mage->weapons.size() > 1 &&
            mage->weapons[1].kind == sim::Weapon::Kind::Remote &&
            mage->weapons[1].mindControl) {
            sim::World w; freshWorld(w);
            int caster = w.spawn(mage, 1000, 1000, 0, 0);
            int v1 = w.spawn(prey, 1000, 1150, 0, 1);
            int v2 = w.spawn(prey, 1060, 1160, 0, 1);
            int v3 = w.spawn(prey, 940, 1140, 0, 1);
            if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
            // A player selects the area spell (Ctrl+W cycles weapons); the sim fires
            // whichever slot is selected, so pick slot 1 as a player would.
            w.setWeapon(caster, 1);
            w.attack(caster, v1, false);
            int flipped = 0;
            for (int i = 0; i < 1200; ++i) {
                w.tick(1.0f / 30.0f);
                flipped = 0;
                for (int id : {v1, v2, v3})
                    if (const sim::Unit* v = w.unit(id); v && v->alive() && v->player == 0) ++flipped;
                if (flipped >= 2) break;
            }
            check(flipped >= 2, "Area Mind Control converts a GROUP",
                  std::to_string(flipped) + " of 3 flipped");
        }
    }

    // ---- 2d. dropped bombs ---------------------------------------------------
    std::printf("[dropped bombs]\n");
    {
        // tarbeak's Egg Bomb is subtype=Dropped: released from the flyer and falls
        // onto the ground beneath, so the bomber must OVERFLY its target rather
        // than shooting from its nominal 200px range.
        const sim::UnitType* bomber = reg.find("tarbeak");
        const sim::UnitType* prey = reg.find("arasword");
        if (!bomber || !prey || bomber->weapons.empty()) std::printf("  (missing defs; skipped)\n");
        else {
            int slot = -1;
            for (size_t i = 0; i < bomber->weapons.size(); ++i)
                if (bomber->weapons[i].kind == sim::Weapon::Kind::Dropped) { slot = int(i); break; }
            check(slot >= 0, "tarbeak carries a Dropped weapon");
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int b = w.spawn(bomber, 1000, 1000, 0, 0);
            int v = w.spawn(prey, 1000, 1500, 0, 1);   // 500px away: well beyond a drop
            if (auto* bu = w.unit(b)) bu->mana = bomber->maxMana;
            w.attack(b, v, false);
            float hp0 = w.unit(v)->hp;
            bool hurt = false; float closest = 1e9f;
            for (int i = 0; i < 1800; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* bb = w.unit(b);
                const sim::Unit* vv = w.unit(v);
                if (!vv) break;
                if (bb && vv) {
                    float d = std::sqrt((bb->x - vv->x) * (bb->x - vv->x) +
                                        (bb->z - vv->z) * (bb->z - vv->z));
                    closest = std::min(closest, d);
                }
                if (!vv->alive() || vv->hp < hp0) { hurt = true; break; }
            }
            // Retail does NOT require an overflight: Dropped reverts to the plain
            // 2D range test and the bomb's velocity is solved so it arrives over
            // the aim point as it lands. So it releases inside its FBI range (200)
            // and the bomb glides in.
            check(closest < 220.0f, "the bomber closed to within its release range",
                  "closest " + std::to_string(int(closest)) + "px");
            check(hurt, "and its dropped bomb glided onto the target");
        }
    }

    // ---- 2e. automatic weapon selection --------------------------------------
    std::printf("[auto weapon selection]\n");
    {
        // Elsin carries Lightning (250px, free, 950), Meteor (400px, 200 mana,
        // 2000) and Earthen Wave (120px, 900 mana, 5000). The sim used to fire slot
        // 0 forever, so he cast Lightning all game and never spent a point of mana.
        const sim::UnitType* king = reg.find("araking");
        const sim::UnitType* prey = reg.find("arasword");
        if (!king || !prey || king->weapons.size() < 3) std::printf("  (missing defs; skipped)\n");
        else {
            auto runFight = [&](bool manualSlot0) {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                cfg.slots[0].team = 0; cfg.slots[1].team = 1;
                sim::setupMatch(w, reg, cfg);
                int c = w.spawn(king, 1000, 1000, 0, 0);
                if (auto* u = w.unit(c)) u->mana = king->maxMana;
                if (manualSlot0) w.setWeapon(c, 0);      // player takes control
                for (int i = 0; i < 40; ++i)
                    w.spawn(prey, 1000.0f + (i % 8) * 30, 1150.0f + (i / 8) * 30, 0, 1);
                bool used[3] = {false, false, false};
                float prev[3] = {0, 0, 0};
                for (int t = 0; t < 900; ++t) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* u = w.unit(c);
                    if (!u) break;
                    for (int sl = 0; sl < 3; ++sl) {
                        if (u->reloads[sl] > prev[sl] + 0.01f) used[sl] = true;
                        prev[sl] = u->reloads[sl];
                    }
                }
                int n = 0; for (bool b : used) if (b) ++n;
                float manaLeft = w.unit(c) ? w.unit(c)->mana : -1;
                return std::pair<int, float>{n, manaLeft};
            };
            auto [autoN, autoMana] = runFight(false);
            // Elsin carries weaponswitching, so retail holds him on ONE weapon --
            // the one the player picked (WEAPON1 by default). An earlier pass here
            // had the sim auto-pick the biggest usable weapon, which made every
            // Monarch, dragon and caster markedly stronger than retail.
            check(king->weaponSwitching, "araking carries weaponswitching");
            check(autoN == 1, "Elsin holds ONE weapon (the player's pick), like retail",
                  std::to_string(autoN) + " of 3 used");
            (void)autoMana;
            auto [manualN, manualMana] = runFight(true);
            check(manualN == 1, "and a Ctrl+W pick is still obeyed",
                  std::to_string(manualN) + " weapon used");
            (void)manualMana;
        }

        // A multi-weapon unit WITHOUT weaponswitching fires every weapon
        // independently -- retail's AA/ground turrets really do work both barrels.
        const sim::UnitType* tower = reg.find("araat");
        if (tower && prey && tower->weapons.size() > 1) {
            check(!tower->weaponSwitching, "araat has NO weaponswitching (fires all)");
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int t = w.spawn(tower, 1000, 1000, 0, 0);
            if (auto* tu = w.unit(t)) tu->mana = tower->maxMana;
            for (int i = 0; i < 20; ++i) w.spawn(prey, 1000.0f + (i % 5) * 25, 1120.0f + (i / 5) * 25, 0, 1);
            bool used[3] = {false, false, false};
            float prev[3] = {0, 0, 0};
            for (int i = 0; i < 900; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* tu = w.unit(t);
                if (!tu) break;
                for (size_t sl = 0; sl < tower->weapons.size() && sl < 3; ++sl) {
                    if (tu->reloads[sl] > prev[sl] + 0.01f) used[sl] = true;
                    prev[sl] = tu->reloads[sl];
                }
            }
            int n = 0; for (bool b2 : used) if (b2) ++n;
            check(n > 1, "araat fires BOTH of its weapons independently",
                  std::to_string(n) + " weapons fired");
        }
    }

    // ---- 3. data-only mission + InitialMission ------------------------------
    std::printf("[data-only mission (.cob optional) + InitialMission]\n");
    {
        const std::string stem = "takmission05_dh";   // Book of Darien chapter 5: no .cob
        bool hasCob = vfs.has("missions/" + stem + ".cob");
        check(!hasCob, "takmission05_dh really ships no .cob");
        sim::World w;
        int human = 0;
        bool ok = sim::setupMission(w, reg, vfs, stem, human);
        check(ok, "the data-only mission loads (used to abort on the missing cob)");
        if (ok) {
            int alive = 0;
            for (const auto& u : w.units()) if (u.alive()) ++alive;
            check(alive > 0, "it spawned its placed units",
                  std::to_string(alive) + " units");
            // start() applies the InitialMission queues; count units holding orders.
            tick(w, 0.5f);
            int withOrders = 0;
            for (const auto& u : w.units())
                if (u.alive() && !u.orders.empty()) ++withOrders;
            check(withOrders > 0, "placed units received InitialMission order queues",
                  std::to_string(withOrders) + " units with orders");
            // And it must stay deterministic/stable for a while.
            tick(w, 3.0f);
            check(true, "ticks without crashing");
        }
    }

    // ---- 4. campaign conditions + the mission GET table ----------------------
    std::printf("[campaign win/lose conditions]\n");
    {
        auto runMission = [&](const char* stem, sim::World& w) {
            int human = 0;
            return sim::setupMission(w, reg, vfs, stem, human);
        };
        auto tickM = [&](sim::World& w, float sec) {
            for (int i = 0; i < int(sec * 30); ++i) {
                w.tick(1.0f / 30.0f);
                if (w.missionOutcome()) break;
            }
        };
        // takmission28_dh is an escort: NPCDERN must cross Z=11 (victory) and losing
        // every Dern is defeat. Neither condition was parsed, so it could not be
        // won OR lost. (It is also one of the 11 missions that ship no .cob.)
        const sim::UnitType* dern = reg.find("npcdern");
        {
            sim::World w;
            if (runMission("takmission28_dh", w) && dern) {
                tickM(w, 2.0f);
                check(w.missionOutcome() == 0, "escort mission does not resolve at the start");
                for (auto& u : w.units())
                    if (u.type == dern) if (auto* p = w.unit(u.id)) p->hp = 0;
                tickM(w, 3.0f);
                check(w.missionOutcome() < 0, "losing every escorted unit is a DEFEAT");
            }
        }
        {
            sim::World w;
            if (runMission("takmission28_dh", w) && dern) {
                tickM(w, 2.0f);
                int moved = 0;
                for (auto& u : w.units())
                    if (u.alive() && u.type == dern)
                        if (auto* p = w.unit(u.id)) { p->z = 5 * 16.0f; ++moved; }
                tickM(w, 2.0f);
                check(moved > 0 && w.missionOutcome() > 0,
                      "getting it across the line is a VICTORY");
            }
        }
        // takmission11_dh: the human plays Taros and must keep TARPRIE2 alive.
        {
            sim::World w;
            const sim::UnitType* pr = reg.find("tarprie2");
            if (runMission("takmission11_dh", w) && pr) {
                tickM(w, 2.0f);   // let the condition arm (it must not fire at t=0)
                for (auto& u : w.units())
                    if (u.type == pr) if (auto* p = w.unit(u.id)) p->hp = 0;
                tickM(w, 3.0f);
                check(w.missionOutcome() < 0, "AllUnitsKilledOfType protects YOUR units");
            }
        }
        // takmission04_ph wins on GET(5, player1) < 5 -- a living-unit count. While
        // the GET table returned 0 for everything, that read as 0 < 5 and the
        // mission declared victory almost immediately.
        {
            sim::World w;
            if (runMission("takmission04_ph", w)) {
                int alive = 0;
                for (const auto& u : w.units()) if (u.alive()) ++alive;
                tickM(w, 5.0f);
                check(alive > 5 && w.missionOutcome() <= 0,
                      "a unit-count victory no longer fires instantly",
                      std::to_string(alive) + " units alive, outcome=" +
                          std::to_string(w.missionOutcome()));
            }
        }
    }

    // --- Random Start Locations -------------------------------------------
    // Retail's room option. Two requirements: the same seed must reproduce the
    // same arrangement on every peer (lockstep), and a different seed must
    // actually move somebody (otherwise the option does nothing).
    std::printf("[random start locations]\n");
    {
        auto startsFor = [&](bool rnd, uint32_t seed) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.randomStarts = rnd;
            cfg.startSeed = seed;
            cfg.slots.resize(4);
            for (int i = 0; i < 4; ++i) { cfg.slots[size_t(i)].used = true;
                                          cfg.slots[size_t(i)].faction = i % 5;
                                          cfg.slots[size_t(i)].team = uint8_t(i); }
            return sim::setupMatch(w, reg, cfg);
        };
        auto fixedA = startsFor(false, 111), fixedB = startsFor(false, 222);
        check(fixedA == fixedB, "fixed starts ignore the seed");
        auto randA = startsFor(true, 111), randA2 = startsFor(true, 111);
        check(randA == randA2, "the same seed reproduces the same arrangement");
        auto randB = startsFor(true, 999);
        bool moved = randA != fixedA || randB != fixedA;
        check(moved, "a shuffled match doesn't just hand back the fixed order");
        bool sameSet = true;
        {
            auto a = fixedA, b = randA;
            std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
            sameSet = a == b;
        }
        check(sameSet, "the shuffle permutes the map's starts, it doesn't invent them");
    }

    // --- Tier-4 "cosmetic lows" ------------------------------------------
    // Several of these are display-only, but they all start with an FBI field that
    // must actually reach UnitType/Weapon -- which is the part that silently rots.
    std::printf("[cosmetic lows: FBI fields reach the registry]\n");
    {
        auto ut = [&](const char* id) { return reg.find(id); };
        if (const auto* wall = ut("arawall"))
            check(wall->noShadow, "arawall carries noshadow");
        if (const auto* harp = ut("verharp"))
            check(harp->floater, "verharp is a floater (retail draws it no shadow)");
        if (const auto* god = ut("aragod")) {
            check(god->canHover, "aragod wades (canhover)");
            check(god->waterline == 40, "and sinks to its waterline",
                  "waterline=" + std::to_string(god->waterline));
        }
        if (const auto* gg = ut("cregod"))
            check(gg->ghost, "the Ghost of Garacaius is a ghost (translucent)");
        // Shadow suppression is the render rule those flags feed.
        if (const auto* wall = ut("arawall"))
            check(!(wall->maxVel > 0) || !wall->noShadow, "a wall is a structure anyway");
        if (const auto* trans = ut("aratrans"))
            check(int(trans->transportDist) == 419,
                  "aratrans loads from its own transportdistance, not a constant 70",
                  std::to_string(int(trans->transportDist)) + "px");
        // turninplacerate: present on most movers, and our default falls back to
        // turnrate rather than retail's 0 (which would freeze a pivot entirely).
        if (const auto* sw = ut("arasword"))
            check(sw->turnInPlaceRate > 0, "a swordsman can pivot on the spot",
                  std::to_string(sw->turnInPlaceRate));
        // AdjustJoy is a repair aura; the Acolyte is the headline carrier.
        if (const auto* pr = ut("arapries")) {
            int joy = 0;
            for (const auto& a : pr->auras)
                if (a.kind == sim::Aura::Kind::Joy && a.radius > 0) ++joy;
            check(joy == 1, "the Acolyte carries one AdjustJoy aura");
            for (const auto& a : pr->auras)
                if (a.kind == sim::Aura::Kind::Joy) {
                    // Retail's falloff is weakest at the emitter, full at the rim.
                    check(a.falloff(0.0f) < a.falloff(a.radius),
                          "the aura is weaker at the centre than at the rim",
                          std::to_string(a.falloff(0.0f)) + " -> " +
                              std::to_string(a.falloff(a.radius)));
                }
        }
    }
    // The repair aura, end to end: a damaged unit next to an Acolyte heals, and the
    // Acolyte's owner pays for it.
    std::printf("[AdjustJoy repair aura]\n");
    {
        const sim::UnitType* pri = reg.find("arapries");
        const sim::UnitType* vic = reg.find("arasword");
        if (pri && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            int pid = w.spawn(pri, 600, 600, 0, 0);
            int vid = w.spawn(vic, 660, 600, 0, 0);   // well inside radius 200
            (void)pid;
            sim::Unit* v = w.unit(vid);
            v->hp = v->type->maxHp * 0.25f;
            float hp0 = v->hp;
            float mana0 = w.player(0).mana;
            for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);   // 2s = two 1Hz pulses
            float hp1 = w.unit(vid)->hp;
            check(hp1 > hp0, "a damaged ally next to an Acolyte is repaired",
                  std::to_string(int(hp0)) + " -> " + std::to_string(int(hp1)));
            check(w.player(0).mana < mana0, "and the Acolyte's owner pays the mana",
                  std::to_string(int(mana0)) + " -> " + std::to_string(int(w.player(0).mana)));
            // An undamaged unit must not be touched (and must still count toward N).
            int fid = w.spawn(vic, 640, 620, 0, 0);
            float full0 = w.unit(fid)->hp;
            for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);
            check(w.unit(fid)->hp <= full0 + 0.01f, "an undamaged ally is left alone");
        }
    }

    // The turn/brake coupling replaced a flat "big turn -> 30% speed" cliff with
    // retail's arc-distance rule. The risk is a unit that brakes forever and never
    // arrives, so test arrival on a straight run AND on a right-angle dogleg.
    std::printf("[turn/brake coupling]\n");
    {
        const sim::UnitType* sw = reg.find("arasword");
        if (sw) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            // heading 0 faces +Z and pi/2 faces +X, so ordering a unit due +X gives
            // an aligned start or a 90-degree departure depending on which we spawn
            // with. Same corridor for both, so terrain can't explain a difference.
            auto runTo = [&](float sz, float head, float secs) {
                int id = w.spawn(sw, 600, sz, head, 0);
                w.order(id, 900, sz, false);
                for (int i = 0; i < int(secs * 30); ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                float dx = u->x - 900.0f, dz = u->z - sz;
                return std::sqrt(dx * dx + dz * dz);
            };
            float aligned = runTo(600, 1.5707963f, 20.0f);
            check(aligned < 48.0f, "a 300px run with an aligned start arrives",
                  std::to_string(int(aligned)) + "px short");
            float turning = runTo(640, 0.0f, 20.0f);
            check(turning < 48.0f, "and so does a 90-degree departure",
                  std::to_string(int(turning)) + "px short");
        }
    }

    // --- missed shots land; lobbers shoot over walls ----------------------
    std::printf("[ballistic: fields + lobbing]\n");
    {
        auto wpn = [&](const char* id, int slot) -> const sim::Weapon* {
            const sim::UnitType* t = reg.find(id);
            if (!t || int(t->weapons.size()) <= slot) return nullptr;
            return &t->weapons[size_t(slot)];
        };
        if (const sim::Weapon* w = wpn("arapult", 0)) {
            check(w->lobPreferred, "the Aramon catapult lobs");
            check(std::abs(w->gravityAdj - 4.25f) < 0.01f,
                  "and carries its own gravityadjustment",
                  std::to_string(w->gravityAdj));
            check(w->aoe > 0, "its cannonball has a blast radius");
        }
        if (const sim::Weapon* w = wpn("arabow", 0))
            check(!w->lobPreferred, "an archer does not lob");
        if (const sim::UnitType* t = reg.find("vermort"))
            check(t->lobs(), "the Veruna mortar lobs (exempt from the LoS gate)");
        if (const sim::UnitType* t = reg.find("arabow"))
            check(!t->lobs(), "an archer is not");
        // dontleadtargets ships only on Dropped bombs, whose weaponvelocity is a
        // fall parameter -- leading on it would throw the aim point off the map.
        int noLead = 0;
        for (const auto& [id, t] : reg.types())
            for (const auto& w : t.weapons)
                if (w.noLead) ++noLead;
        check(noLead > 0, "dontleadtargets reaches the registry",
              std::to_string(noLead) + " weapon blocks");
    }
    // A missed splash shell must crater. Fire a catapult, wait for the shell to be
    // in the air, then move the target out from under it -- the shell must still
    // land where it was aimed and splash whatever is standing there.
    // Every faction's wall must actually block. Two of the four declare canmove=1
    // with no velocity, so a canMove gate left them blocking nothing at all.
    std::printf("[walls block, all four factions]\n");
    {
        for (const char* id : {"arawall", "crewall", "verwall", "tarwall"}) {
            const sim::UnitType* t = reg.find(id);
            if (!t) continue;
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            // Find open ground rather than assuming a cell: Inner Circle is mostly
            // blocked terrain and a hardcoded cell tests nothing.
            int cx = -1, cz = -1;
            for (int j = 30; j < 70 && cx < 0; ++j)
                for (int i = 30; i < 70; ++i)
                    if (w.nav().walkable(i, j) && w.nav().walkable(i + 1, j) &&
                        w.nav().walkable(i, j + 1) && w.nav().walkable(i + 1, j + 1)) {
                        cx = i; cz = j; break;
                    }
            if (cx < 0) { check(false, "found open ground to build a wall on"); continue; }
            float wx = float(cx) * 16 + 8 + 16, wz = float(cz) * 16 + 8 + 16;
            bool before = w.nav().walkable(cx, cz);
            w.spawn(t, wx, wz, 0, 0);
            sim::blockFootprint(w.nav(), *t, wx, wz, true);
            bool after = w.nav().walkable(cx, cz);
            check(before && !after, "the wall blocks its footprint",
                  std::string(id) + " (canmove=" + (t->canMove ? "1" : "0") + ")");
        }
    }
    std::printf("[a missed shell craters]\n");
    {
        const sim::UnitType* pult = reg.find("arapult");
        const sim::UnitType* vic = reg.find("arasword");
        if (pult && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int gun = w.spawn(pult, 600, 900, 0, 0);
            int mark = w.spawn(vic, 900, 900, 0, 1);
            int bystander = w.spawn(vic, 916, 900, 0, 1);   // beside the aim point
            float by0 = w.unit(bystander)->hp;
            w.attack(gun, mark, false);
            bool flew = false, moved = false;
            for (int i = 0; i < 30 * 20; ++i) {
                w.tick(1.0f / 30.0f);
                if (!moved && !w.projectiles().empty()) {
                    flew = true;
                    // Yank the target well clear, so the shell cannot connect.
                    sim::Unit* m = w.unit(mark);
                    m->x = 600; m->z = 1400;
                    moved = true;
                }
            }
            check(flew, "the catapult actually fired");
            float by1 = w.unit(bystander)->hp;
            check(by1 < by0, "the shell still lands and splashes the aim point",
                  std::to_string(int(by0)) + " -> " + std::to_string(int(by1)));
        }
    }
    // ...but a shot that CONNECTS must not also crater on the same tick (the
    // life<=0 expiry and the direct hit can coincide -- that is what `spent` guards).
    std::printf("[a hit does not double-dip]\n");
    {
        const sim::UnitType* pult = reg.find("arapult");
        const sim::UnitType* vic = reg.find("araking");   // tough enough to survive
        if (pult && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int gun = w.spawn(pult, 600, 1100, 0, 0);
            int sitting = w.spawn(vic, 900, 1100, 0, 1);   // stationary: no lead, direct hit
            float hp0 = w.unit(sitting)->hp;
            w.attack(gun, sitting, false);
            // Long enough for exactly one shell to be fired and to land, but well
            // inside the 6.5s reload so a second can't confuse the total.
            for (int i = 0; i < 30 * 6; ++i) w.tick(1.0f / 30.0f);
            float lost = hp0 - w.unit(sitting)->hp;
            const sim::Weapon& cw = pult->weapons[0];
            float once = cw.damageVs(vic);
            check(lost <= once * 1.60f,
                  "one shell deals about one shell's damage, not two",
                  std::to_string(int(lost)) + " vs " + std::to_string(int(once)));
        }
    }

    // --- units are solid --------------------------------------------------
    // The dangerous change in this area is not "does blocking work" but "does an
    // army still move". Test both, and test the corridor case that deadlocks first.
    std::printf("[units are solid]\n");
    {
        const sim::UnitType* sw = reg.find("arasword");
        if (sw) {
            // (a) A parked body blocks: walk one unit into a stationary one and
            //     assert it cannot pass through.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                sim::setupMatch(w, reg, cfg);
                int blocker = w.spawn(sw, 700, 600, 0, 0);
                int walker = w.spawn(sw, 640, 600, 0, 0);
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);   // settle
                w.order(walker, 800, 600, false);                          // straight through
                // Sample every tick, not just the end: a single final position
                // cannot tell "went around" from "went through and kept going",
                // and that is exactly how a 13px interpenetration hid behind a
                // passing end-state check once.
                const float bx0 = w.unit(blocker)->x, bz0 = w.unit(blocker)->z;
                const float touch =
                    float(std::max(sw->footX, sw->footZ)) * 8.0f * 2.0f;   // px, centre to centre
                float closest = 1e9f;
                for (int i = 0; i < 30 * 10; ++i) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* m2 = w.unit(walker);
                    if (!m2 || !m2->alive()) break;
                    closest = std::min(closest, std::max(std::fabs(m2->x - bx0),
                                                         std::fabs(m2->z - bz0)));
                }
                check(closest >= touch - 1.0f,
                      "a walker never penetrates a parked body's footprint",
                      "closest approach " + std::to_string(int(closest)) +
                          "px, footprints touch at " + std::to_string(int(touch)));
                const sim::Unit* b = w.unit(blocker);
                const sim::Unit* m = w.unit(walker);
                // The discriminating assertion: the walker was sent to x=800, PAST
                // the blocker at 700. With only separation it shoulders through and
                // arrives; solid, it is still on the near side or squeezing round.
                check(m->x < 800.0f - 40.0f,
                      "a parked body actually stops a walker (not just spaces it)",
                      "walker x=" + std::to_string(int(m->x)) +
                          " blocker x=" + std::to_string(int(b->x)));
            }
            // (b) An army still moves. 24 units ordered across open ground must
            //     nearly all arrive -- this is the regression that matters.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                sim::setupMatch(w, reg, cfg);
                std::vector<int> army;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 6; ++i)
                        army.push_back(w.spawn(sw, 600.0f + float(i) * 20.0f,
                                               600.0f + float(j) * 20.0f, 0, 0));
                for (int id : army) w.order(id, 900, 700, false);
                for (int i = 0; i < 30 * 40; ++i) w.tick(1.0f / 30.0f);
                int arrived = 0;
                for (int id : army) {
                    const sim::Unit* u = w.unit(id);
                    float dx = u->x - 900.0f, dz = u->z - 700.0f;
                    if (std::sqrt(dx * dx + dz * dz) < 120.0f) ++arrived;
                }
                check(arrived >= int(army.size()) * 3 / 4,
                      "an army of 24 still reaches its destination",
                      std::to_string(arrived) + "/" + std::to_string(army.size()));
            }
            // (c) Two columns marching THROUGH each other must not lock. This is
            //     deadlock mode #1, and the reason occupancy records only parked
            //     bodies -- two moving units can never block one another.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                sim::setupMatch(w, reg, cfg);
                std::vector<int> east, west;
                for (int i = 0; i < 6; ++i) {
                    east.push_back(w.spawn(sw, 620.0f + float(i) * 18.0f, 660, 0, 0));
                    west.push_back(w.spawn(sw, 880.0f - float(i) * 18.0f, 660, 0, 0));
                }
                for (int id : east) w.order(id, 900, 660, false);
                for (int id : west) w.order(id, 600, 660, false);
                for (int i = 0; i < 30 * 40; ++i) w.tick(1.0f / 30.0f);
                int through = 0;
                for (int id : east) if (w.unit(id)->x > 820.0f) ++through;
                for (int id : west) if (w.unit(id)->x < 680.0f) ++through;
                check(through >= 8, "two columns pass through each other",
                      std::to_string(through) + "/12 got past");
            }
        }
    }

    // Mobile units take their footprint from the movement class, not their own FBI:
    // 125 of 152 movers declare no footprintx at all, and not one is 1x1.
    std::printf("[movementclass footprints]\n");
    {
        struct Want { const char* id; int f; const char* cls; };
        const Want want[] = {
            {"arasword", 2, "GROUND2"}, {"araarch", 2, "GROUND2"},
            {"arapult",  4, "GROUND4"},
            // The Trebuchet declares footprintx=7 in its own FBI and has NO
            // movement class -- the one case where the FBI is the source, and a
            // reminder that footprints are not capped by the class table.
            {"aratre",   7, "(own FBI)"},
        };
        for (const auto& wt : want)
            if (const sim::UnitType* t = reg.find(wt.id))
                check(t->footX == wt.f && t->footZ == wt.f,
                      "a mover inherits its class footprint",
                      std::string(wt.id) + " " + std::to_string(t->footX) + "x" +
                          std::to_string(t->footZ) + " (want " + wt.cls + ")");
        int ones = 0, movers = 0;
        for (const auto& [id, t] : reg.types()) {
            if (t.maxVel <= 0 || t.canFly) continue;
            ++movers;
            if (t.footX <= 1) ++ones;
        }
        check(movers > 100 && ones == 0,
              "no mobile ground/water unit is left at 1x1",
              std::to_string(ones) + " of " + std::to_string(movers) + " still 1x1");
    }

    // A '.' cell is not part of a building's footprint: retail runs no test on it.
    // The Keep has two full rows of them, so a tree parked there used to deny a
    // placement retail allows.
    std::printf("[yardmap-aware placement]\n");
    {
        const sim::UnitType* keep = reg.find("arakeep");
        if (keep && !keep->yardMap.empty()) {
            int dots = 0;
            for (char c : keep->yardMap) if (c == '.' || c == ' ') ++dots;
            check(dots > 0, "the Keep's yardmap has non-footprint cells",
                  std::to_string(dots) + " of " +
                      std::to_string(keep->footX * keep->footZ));
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            // Find open ground the Keep fits on, then block one cell that the
            // yardmap marks '.' and assert the placement still stands.
            int bx = -1, bz = -1;
            for (int z = 20; z < 90 && bx < 0; ++z)
                for (int x = 20; x < 90; ++x) {
                    float wx = float(x) * 16 + 8, wz = float(z) * 16 + 8;
                    if (w.canPlace(keep, wx, wz)) { bx = x; bz = z; break; }
                }
            if (bx < 0) { check(false, "found somewhere the Keep fits"); }
            else {
                // Locate a '.' cell in the yardmap and block exactly that world cell.
                int dotI = -1, dotJ = -1;
                for (int j = 0; j < keep->footZ && dotI < 0; ++j)
                    for (int i = 0; i < keep->footX; ++i)
                        if (keep->yardMap[size_t(j) * keep->footX + i] == '.') {
                            dotI = i; dotJ = j; break;
                        }
                int ox = bx - keep->footX / 2, oz = bz - keep->footZ / 2;
                w.blockCells(ox + dotI, oz + dotJ, 1, 1, true);
                float wx = float(bx) * 16 + 8, wz = float(bz) * 16 + 8;
                check(w.canPlace(keep, wx, wz),
                      "a blocked '.' cell does not deny the placement",
                      "cell " + std::to_string(dotI) + "," + std::to_string(dotJ));
                // ...but a blocked 'o' cell must still deny it.
                int solidI = -1, solidJ = -1;
                for (int j = 0; j < keep->footZ && solidI < 0; ++j)
                    for (int i = 0; i < keep->footX; ++i) {
                        char c = keep->yardMap[size_t(j) * keep->footX + i];
                        if (c != '.' && c != ' ') { solidI = i; solidJ = j; break; }
                    }
                w.blockCells(ox + solidI, oz + solidJ, 1, 1, true);
                check(!w.canPlace(keep, wx, wz),
                      "a blocked footprint cell still denies it");
            }
        }
    }

    // ---- queued orders on ordinary mobile units -----------------------------
    // A single player order can expand into a whole route, so the order queue
    // interleaves pathfinding waypoints with the goals the player actually asked
    // for. Anything that treats orders.back() as "the destination" therefore
    // deletes every leg the player lined up behind the current one.
    {
        auto freshWorld = [&](sim::World& w) {
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
        };
        const sim::UnitType* sword = reg.find("arasword");
        check(sword && sword->canMove, "arasword is a plain mobile non-builder");
        if (sword && sword->canMove) {
            // Two queued legs: out to A, then on to B. The unit must visit A
            // BEFORE B -- not cut straight to the last one.
            sim::World w; freshWorld(w);
            float sx = 1000, sz = 1000;
            legalSpot(w, sword, sx, sz);
            // Both waypoints must be somewhere the unit can stand: order() now
            // snaps an unstandable destination to the nearest cell that fits,
            // and two waypoints on rock can snap to the same place, which makes
            // "visited A before B" meaningless.
            // Put the whole L-shaped route inside one open apron. Individually
            // legal waypoints are not enough: they can sit in separate pockets
            // with rock between, and then "walks the first leg" fails for
            // reasons that have nothing to do with queueing.
            {
                const sim::NavGrid& g = w.navFor(sword);
                bool placed = false;
                for (int cz2 = 30; cz2 < g.height() - 30 && !placed; cz2 += 2)
                    for (int cx2 = 30; cx2 < g.width() - 30 && !placed; cx2 += 2) {
                        bool clear = true;
                        for (int dz = 0; dz <= 26 && clear; ++dz)
                            for (int dx = 0; dx <= 26 && clear; ++dx)
                                if (!g.fits(cx2 + dx, cz2 + dz, 2)) clear = false;
                        if (!clear) continue;
                        sx = float(cx2) * 16 + 8;
                        sz = float(cz2) * 16 + 8;
                        placed = true;
                    }
            }
            const float ax = sx, az = sz + 400;         // leg 1: due south
            const float bx = sx + 400, bz = sz + 400;   // leg 2: then east
            int id = w.spawn(sword, sx, sz, 0, 0);
            w.order(id, ax, az, false);
            size_t afterFirst = w.unit(id)->orders.size();
            w.order(id, bx, bz, true);            // QUEUED behind it
            check(w.unit(id)->orders.size() > afterFirst,
                  "a queued move appends to the queue instead of replacing it");

            bool reachedA = false, reachedBBeforeA = false;
            for (int i = 0; i < 30 * 90 && !reachedBBeforeA; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                if (!u || !u->alive()) break;
                float da = detmathLen(u->x - ax, u->z - az);
                float db = detmathLen(u->x - bx, u->z - bz);
                if (da < 40) reachedA = true;
                if (db < 40) { if (!reachedA) reachedBBeforeA = true; break; }
                if (reachedA && db < 40) break;
            }
            check(reachedA, "the unit walks the FIRST queued leg");
            check(!reachedBBeforeA, "and does not skip straight to the last one");

            // THE REGRESSION. The progress-based unstick measures progress
            // against orders.back() -- the LAST queued leg. Send a unit out and
            // then back, and walking the first leg moves it AWAY from the last
            // one, so after ~2s it looks permanently stuck: the watchdog fires,
            // routes straight to the final leg, and the outbound leg is gone.
            sim::World w2; freshWorld(w2);
            const float homeX = 448, homeZ = 2624;   // open ground with a real route
            const float outX = 848, outZ = 2624;     // leg 1: east, AWAY from...
            const float backX = homeX, backZ = homeZ; // leg 2: ...back where we began
            int id2 = w2.spawn(sword, homeX, homeZ, 0, 0);
            w2.order(id2, outX, outZ, false);
            w2.order(id2, backX, backZ, true);
            bool visitedOut = false;
            for (int i = 0; i < 30 * 90; ++i) {
                w2.tick(1.0f / 30.0f);
                const sim::Unit* u = w2.unit(id2);
                if (!u || !u->alive()) break;
                if (detmathLen(u->x - outX, u->z - outZ) < 48) { visitedOut = true; break; }
                if (u->orders.empty()) break;       // finished without ever going out
            }
            check(visitedOut,
                  "an out-and-back queue walks the outbound leg (the unstick watchdog "
                  "must not mistake it for being stuck against the RETURN leg)");
        }
    }

    // ---- fireatwillrandom: missile troops spread their fire ------------------
    // Retail scores auto-acquired candidates n = dist^2/damage and keeps the
    // lowest of rand(n)/2 + rand(n); the flag swaps dist^2 for INT_MAX, so the
    // pick stops preferring the nearest body. A rank of archers should therefore
    // cover noticeably more of a crowd than the same number of swordsmen.
    {
        auto rank = [&](const char* who, int& distinct, int& maxOnOne) {
            const sim::UnitType* sh = reg.find(who);
            const sim::UnitType* prey = reg.find("arasword");
            distinct = maxOnOne = 0;
            if (!sh || !prey) return;
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            // Two ranks rather than one long line, spaced so no body starts
            // inside another (a footprint is 32px; the old 20/22px pitch spawned
            // them INTERPENETRATING). That used to be invisible because the
            // separation pass shoved them apart over the first few ticks, so the
            // spread this test reads was partly an artifact of that shoving
            // rather than of the acquisition rule it is about. Retail does not
            // spawn bodies inside each other; neither should the fixture. The
            // block keeps roughly the old footprint so the range picture holds.
            for (int i = 0; i < 12; ++i)
                w.spawn(sh, 900.0f + float(i % 6) * 36, 2500.0f + float(i / 6) * 36, 0, 0);
            for (int i = 0; i < 8; ++i)
                w.spawn(prey, 950.0f + float(i % 4) * 36, 2760.0f + float(i / 4) * 36, 0, 1);
            for (int i = 0; i < 10; ++i) w.tick(1.0f / 30.0f);
            std::map<int, int> tally;
            for (const auto& u : w.units()) {
                if (!u.alive() || u.player != 0 || u.orders.empty()) continue;
                if (int t = u.orders.front().targetId) tally[t]++;
            }
            distinct = int(tally.size());
            for (auto& [t, n] : tally) maxOnOne = std::max(maxOnOne, n);
        };
        const sim::UnitType* archer = reg.find("araarch");
        check(archer && archer->fireAtWillRandom, "araarch parses fireatwillrandom");
        const sim::UnitType* sword = reg.find("arasword");
        check(sword && !sword->fireAtWillRandom, "arasword does not carry it");
        int aDistinct = 0, aMax = 0, sDistinct = 0, sMax = 0;
        rank("araarch", aDistinct, aMax);
        rank("arasword", sDistinct, sMax);
        check(aDistinct > sDistinct,
              "12 archers cover more of an 8-strong crowd than 12 swordsmen",
              "archers " + std::to_string(aDistinct) + " targets vs swordsmen " +
                  std::to_string(sDistinct));
        check(aMax <= sMax,
              "and pile onto one body no harder than the swordsmen do",
              "archers " + std::to_string(aMax) + " on one vs swordsmen " +
                  std::to_string(sMax));
        // The scatter is deterministic: same world, same draws, same answer.
        int d2 = 0, m2 = 0;
        rank("araarch", d2, m2);
        check(d2 == aDistinct && m2 == aMax,
              "the scatter is deterministic (identical across two identical runs)");
    }

    // ---- standing orders: the stance buttons write TWO retail fields ---------
    {
        auto freshWorld = [&](sim::World& w) {
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
        };
        // standingunitorder = 1 (Defensive): move 0 / fire 2.
        const sim::UnitType* archer = reg.find("araarch");
        check(archer && archer->defaultMove == 0 && archer->defaultFire == 2,
              "araarch (standingunitorder=1) defaults to hold-position + fire-at-will");
        // standingunitorder = 2 (Offensive): move 1 / fire 2 -- a LEASHED chase.
        const sim::UnitType* knight = reg.find("araknigh");
        check(knight && knight->defaultMove == 1 && knight->defaultFire == 2,
              "araknigh (standingunitorder=2) defaults to a leashed chase");
        // standingunitorder = 0 (Passive): move 0 / fire 0 -- holds fire entirely.
        const sim::UnitType* spy = reg.find("araspy");
        check(spy && spy->defaultMove == 0 && spy->defaultFire == 0,
              "araspy (standingunitorder=0) defaults to holding fire");
        // No key at all -> the sentinel path: roam + fire at will. Worth noting
        // what this set turns out to be: almost every unit WITHOUT the key is a
        // structure (Keep, Watch Tower, wall, Mana Amplifier...) plus a couple of
        // flyers. So in retail essentially no mobile ground unit roams without a
        // leash -- they are all either hold-position or leashed at 500.
        const sim::UnitType* flyer = reg.find("arafly");
        check(flyer && flyer->defaultMove == 2 && flyer->defaultFire == 2,
              "arafly (no standingunitorder) defaults to unlimited roam");
        const sim::UnitType* sword = reg.find("arasword");
        check(sword && sword->defaultMove == 1,
              "arasword (standingunitorder=2) is leashed, not roaming");

        // A hold-position unit shoots what comes to it but never walks over.
        if (archer && sword) {
            sim::World w; freshWorld(w);
            int a = w.spawn(archer, 1000, 2600, 0, 0);
            check(w.unit(a)->moveState == 0 && w.unit(a)->fireState == 2,
                  "the spawned archer carries its type's standing orders");
            float far = float(archer->maxRange()) + 260.0f;
            int prey = w.spawn(sword, 1000 + far, 2600, 0, 1);
            (void)prey;
            float startX = w.unit(a)->x;
            for (int i = 0; i < 30 * 8; ++i) w.tick(1.0f / 30.0f);
            const sim::Unit* au = w.unit(a);
            check(au && std::fabs(au->x - startX) < 24.0f,
                  "a hold-position archer does NOT walk to an out-of-range enemy",
                  "moved " + std::to_string(au ? au->x - startX : 0.0f) + "px");

            // A leashed unit advances on something inside its leash...
            sim::World w2; freshWorld(w2);
            int a2 = w2.spawn(sword, 1000, 2600, 0, 0);   // arasword: move 1, leash 500
            w2.spawn(sword, 1300, 2600, 0, 1);            // 300px: inside the leash
            check(w2.unit(a2)->moveState == 1, "the swordsman is leashed");
            float sx2 = w2.unit(a2)->x;
            for (int i = 0; i < 30 * 8; ++i) w2.tick(1.0f / 30.0f);
            check(w2.unit(a2) && w2.unit(a2)->x - sx2 > 24.0f,
                  "a leashed unit advances on an enemy inside its leash",
                  "moved " + std::to_string(w2.unit(a2) ? w2.unit(a2)->x - sx2 : 0.0f) + "px");

            // ...and stays put for one beyond it.
            sim::World w4; freshWorld(w4);
            int a4 = w4.spawn(sword, 1000, 2600, 0, 0);
            w4.spawn(sword, 1000 + sword->leash + 220.0f, 2600, 0, 1);
            float sx4 = w4.unit(a4)->x;
            for (int i = 0; i < 30 * 8; ++i) w4.tick(1.0f / 30.0f);
            check(w4.unit(a4) && std::fabs(w4.unit(a4)->x - sx4) < 24.0f,
                  "but will not break its leash for one beyond it",
                  "moved " + std::to_string(w4.unit(a4) ? w4.unit(a4)->x - sx4 : 0.0f) + "px");

            // The Offensive button writes both fields.
            sim::World w5; freshWorld(w5);
            int a5 = w5.spawn(archer, 1000, 2600, 0, 0);
            w5.setStance(a5, 0);
            check(w5.unit(a5)->moveState == 1 && w5.unit(a5)->fireState == 2,
                  "the Offensive button writes move=1 AND fire=2");

            // Passive holds fire: no target is ever acquired.
            sim::World w3; freshWorld(w3);
            int a3 = w3.spawn(archer, 1000, 2600, 0, 0);
            w3.spawn(sword, 1000 + 40, 2600, 0, 1);   // right next to it
            w3.setStance(a3, 2);   // Passive
            check(w3.unit(a3)->fireState == 0, "the Passive button writes fire=0");
            bool acquired = false;
            for (int i = 0; i < 30 * 5 && !acquired; ++i) {
                w3.tick(1.0f / 30.0f);
                const sim::Unit* u3 = w3.unit(a3);
                if (u3 && !u3->orders.empty() && u3->orders.front().targetId) acquired = true;
            }
            check(!acquired, "a Passive unit never auto-acquires, even point blank");
        }
    }

    // ---- VTOL_standby: an idle flyer finds somewhere to put down -------------
    {
        const sim::UnitType* fly = reg.find("arafly");
        check(fly && fly->canFly && fly->vtolStandby,
              "arafly parses defaultmissiontype = VTOL_standby");
        if (fly && fly->vtolStandby) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            // Park it over a cell it could not land on (water or blocked).
            float bx = 0, bz = 0; bool bad = false;
            for (int z = 200; z < 3000 && !bad; z += 16)
                for (int x = 200; x < 3000; x += 16)
                    if (!w.navFor(fly).walkable(x / 16, z / 16)) {
                        bx = float(x); bz = float(z); bad = true; break;
                    }
            check(bad, "the map has a cell no flyer could land on");
            if (bad) {
                int id = w.spawn(fly, bx, bz, 0, 0);
                for (int i = 0; i < 30 * 20; ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                bool ok = u && w.navFor(fly).walkable(int(u->x) / 16, int(u->z) / 16);
                check(ok, "an idle flyer relocates off a spot it cannot land on",
                      u ? "ended at " + std::to_string(int(u->x)) + "," +
                              std::to_string(int(u->z)) : "gone");
                // And then it STAYS: the search must not re-trigger every tick and
                // leave the thing shuffling for the rest of the game.
                float sx = u ? u->x : 0, sz = u ? u->z : 0;
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u2 = w.unit(id);
                check(u2 && std::fabs(u2->x - sx) < 2.0f && std::fabs(u2->z - sz) < 2.0f,
                      "...and then settles, instead of shuffling forever");
            }
        }
    }

    // ---- unitstandorders: which types have a stance at all -------------------
    {
        const sim::UnitType* keep = reg.find("arakeep");
        const sim::UnitType* cow = reg.find("lifcow");
        const sim::UnitType* sword = reg.find("arasword");
        check(keep && !keep->canSetStance, "the Keep has no combat stance");
        check(cow && !cow->canSetStance, "nor does a cow");
        check(sword && sword->canSetStance, "an ordinary soldier does");

        // The sim refuses to set one, not just the HUD -- a modified client must
        // not be able to hand a stance to something retail would not offer it for.
        if (cow) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int c = w.spawn(cow, 1000, 2600, 0, 0);
            int before = w.unit(c)->stance;
            w.setStance(c, before == 0 ? 2 : 0);
            check(w.unit(c)->stance == before, "and the sim refuses to change it");
        }

        // The gate is NOT cosmetic, which is worth pinning down: three shipped
        // types are mobile AND armed yet have no stance -- npcbeg2, npcfarm2 and
        // npcpeas2, the armed variants of the civilian Beggar/Farmer/Peasant (230
        // damage apiece). Retail greys their buttons out; a "mobile and armed"
        // guess would have offered them. Their weapons are declared as a [WEAPON1]
        // SECTION rather than a weapon1= key, which is exactly why a quick grep
        // over the FBIs makes them look unarmed.
        std::vector<std::string> armedWithoutStance;
        for (const auto& [id, t] : reg.types())
            if (!t.canSetStance && t.maxVel > 0 && t.weapon.damage > 0)
                armedWithoutStance.push_back(id);
        std::sort(armedWithoutStance.begin(), armedWithoutStance.end());
        std::string joined;
        for (const auto& n : armedWithoutStance) joined += (joined.empty() ? "" : ",") + n;
        check(joined == "npcbeg2,npcfarm2,npcpeas2",
              "the armed civilians are the types the stance gate actually changes",
              "got [" + joined + "]");
        for (const char* id : {"npcbeg2", "npcfarm2", "npcpeas2"}) {
            const sim::UnitType* t = reg.find(id);
            check(t && !t->canSetStance && t->weapon.damage > 0,
                  "armed civilian with no stance", id);
        }
    }

    // ---- moves and builds queue as ONE sequence ------------------------------
    // Builds used to live in a separate list, so "go here, build that, go there"
    // ran as "go here, go there, (never build)" -- the build sat in the other
    // queue with nothing left to drain it.
    {
        sim::TypeRegistry breg;
        breg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
        breg.loadDir(vfs, "units/");
        breg.loadBuildTree(vfs, "canbuild/");
        const sim::UnitType* mon = breg.find("zonhunt");   // the Zhon monarch; a flyer
        const auto& menu = breg.buildable("zonhunt");
        check(mon && mon->isBuilder && !menu.empty(), "zonhunt is a builder with a menu");
        if (mon && !menu.empty()) {
            const sim::UnitType* bt = breg.find(menu[0]);
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, breg, cfg);
            w.player(0).mana = 100000;
            int id = w.spawn(mon, 800, 2600, 0, 0);
            w.order(id, 1100, 2600, false);             // move
            w.queueBuild(id, bt, 1150, 2700, true);     // ...then build
            w.order(id, 1400, 2600, true);              // ...then move again
            check(w.unit(id)->orders.size() == 3,
                  "move + build + move is ONE queue of three",
                  std::to_string(w.unit(id)->orders.size()) + " entries");

            bool built = false, sawSite = false;
            for (int i = 0; i < 30 * 60; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                if (!u) break;
                if (u->buildSiteId != 0) sawSite = true;
            }
            for (const auto& un : w.units())
                if (un.alive() && un.type == bt && !un.underConstruction) built = true;
            check(sawSite, "the queued build actually starts");
            check(built, "and finishes");
            const sim::Unit* u = w.unit(id);
            check(u && u->x > 1350.0f,
                  "and THEN the builder goes on to the move queued behind it",
                  u ? "ended at x=" + std::to_string(int(u->x)) : "gone");
            check(u && u->orders.empty(),
                  "with nothing left stuck in the queue");

            // And the pure-build chain: three buildings, no movement between them.
            // Pinned BEFORE the queues were merged, so the merge cannot quietly
            // break the case that already worked.
            sim::World w2;
            sim::setupMatch(w2, breg, cfg);
            w2.player(0).mana = 200000;
            int m2 = w2.spawn(mon, 800, 2600, 0, 0);
            const float sites[3][2] = {{1000, 2700}, {1120, 2700}, {1240, 2700}};
            for (int i = 0; i < 3; ++i)
                w2.queueBuild(m2, bt, sites[i][0], sites[i][1], i != 0);
            int done = 0;
            for (int i = 0; i < 30 * 180; ++i) {
                w2.tick(1.0f / 30.0f);
                done = 0;
                for (const auto& un : w2.units())
                    if (un.alive() && un.type == bt && !un.underConstruction) ++done;
                if (done == 3) break;
            }
            check(done == 3, "three buildings queued back to back all get built",
                  std::to_string(done) + " of 3");
            const sim::Unit* mu = w2.unit(m2);
            // Not "orders empty": zonhunt is a flyer, and once the last building
            // is up it is standing on a cell that building now blocks, so the
            // VTOL standby correctly issues a move to somewhere it CAN land.
            check(mu && !sim::World::hasQueuedBuild(*mu) && mu->buildSiteId == 0,
                  "and the builder ends with no construction left queued",
                  mu ? "orders=" + std::to_string(mu->orders.size()) +
                           " site=" + std::to_string(mu->buildSiteId) +
                           (mu->orders.empty() ? "" :
                            (mu->orders.front().buildType ? " front=BUILD" : " front=move"))
                     : "gone");
        }
    }

    // ---- an area reclaim queues like anything else ---------------------------
    {
        sim::TypeRegistry breg;
        breg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
        breg.loadDir(vfs, "units/");
        breg.loadBuildTree(vfs, "canbuild/");
        const sim::UnitType* mon = breg.find("zonhunt");
        const auto& menu = breg.buildable("zonhunt");
        if (mon && !menu.empty()) {
            const sim::UnitType* bt = breg.find(menu[0]);
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, breg, cfg);
            w.player(0).mana = 200000;
            int id = w.spawn(mon, 800, 2600, 0, 0);
            std::vector<int> feats;
            for (const auto& f : w.features()) {
                if (!f.alive || f.work <= 0) continue;
                float dx = f.x - 900, dz = f.z - 2700;
                if (dx * dx + dz * dz < 600 * 600) feats.push_back(f.id);
                if (feats.size() >= 3) break;
            }
            check(feats.size() == 3, "three reclaimable features near the builder");
            if (feats.size() == 3) {
                // A build first, then the area reclaim queued behind it -- the exact
                // shape that used to leave the reclaims parked forever, because the
                // old reclaimQueue only ever drained from inside tickReclaim and so
                // never started unless a reclaim was ALREADY running.
                w.queueBuild(id, bt, 1000, 2700, false);
                for (int fid : feats) w.reclaim(id, fid, true);
                int queued = 0;
                for (const auto& o : w.unit(id)->orders) if (o.reclaimFeat) ++queued;
                check(queued == 3, "all three reclaims are in the ONE order queue",
                      std::to_string(queued) + " queued");
                for (int i = 0; i < 30 * 150; ++i) w.tick(1.0f / 30.0f);
                int left = 0;
                for (int fid : feats) { const auto* f = w.feature(fid); if (f && f->alive) ++left; }
                check(left == 0, "and every one of them gets reclaimed",
                      std::to_string(left) + " still standing");
            }
        }
    }

    // ---- self-destruct actually kills -------------------------------------
    // Ctrl+Shift+D arms a 5s countdown whose expiry zeroes hp. The death sweep
    // runs EARLIER in the same per-unit tick than the expiry, so the kill is only
    // noticed next tick -- and healtime regen runs a few lines AFTER the expiry.
    // A regenerating unit therefore healed straight back off zero and survived,
    // which is nearly every unit: 209 of 213 shipped FBIs have healtime > 0.
    std::printf("[self-destruct]\n");
    {
        const sim::UnitType* regen = nullptr;
        const sim::UnitType* norgen = nullptr;
        for (const auto& [tid, t] : reg.types()) {
            if (t.maxVel <= 0) continue;                  // movers only: easy to spawn
            if (!regen && t.healTime > 0) regen = &t;
            if (!norgen && t.healTime <= 0) norgen = &t;
        }
        if (!regen) std::printf("  (no regenerating unit type; skipped)\n");
        else {
            struct Case { const sim::UnitType* t; const char* label; };
            std::vector<Case> cases{{regen, "a REGENERATING unit"}};
            if (norgen) cases.push_back({norgen, "a non-regenerating unit"});
            for (const auto& c : cases) {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs;
                cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                cfg.slots[0].team = 0; cfg.slots[1].team = 1;
                sim::setupMatch(w, reg, cfg);
                int id = w.spawn(c.t, 500, 500, 0, 0);
                if (id <= 0 || !w.unit(id)) continue;
                w.destroy(id);
                check(w.unit(id)->selfDestructT > 0.0f, (std::string(c.label) + ": countdown armed").c_str());
                tick(w, 8.0f);   // 5s countdown + slack
                const sim::Unit* u = w.unit(id);
                check(u && !u->alive(), (std::string(c.label) + " (" + c.t->id + ") is DEAD").c_str(),
                      u ? ("hp=" + std::to_string(u->hp)) : "gone");
            }
            // A self-destruct next to a friendly healer must still kill. alive()
            // reads deadFor, not hp, so a unit sitting at zero hp still looks alive
            // to every heal path until the death sweep runs -- and the AdjustJoy
            // aura pulses AFTER that loop, so it used to resurrect the corpse.
            const sim::UnitType* healer = nullptr;
            for (const auto& [tid, t] : reg.types())
                for (const auto& a : t.auras)
                    if (a.kind == sim::Aura::Kind::Joy && a.radius > 0 && a.amount > 0) {
                        if (!healer) healer = &t;
                    }
            if (!healer) std::printf("  (no AdjustJoy emitter in the data; aura case skipped)\n");
            else {
                // The aura pulses at 1 Hz, so the resurrection only bites when the
                // countdown expires ON a pulse tick. Sweep every phase so one case
                // is guaranteed to line up -- otherwise this passes by luck and
                // tests nothing (it did, until the phase sweep was added).
                int survived = 0;
                std::string worst;
                for (int phase = 0; phase < 30; ++phase) {
                    sim::World w;
                    sim::MatchConfig cfg;
                    cfg.vfs = &vfs;
                    cfg.mapPath = kMap;
                    cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                    cfg.slots[0].team = 0; cfg.slots[1].team = 1;
                    sim::setupMatch(w, reg, cfg);
                    w.spawn(healer, 500, 500, 0, 0);              // the aura emitter
                    int vid = w.spawn(regen, 520, 500, 0, 0);     // well inside its radius
                    if (vid <= 0 || !w.unit(vid)) break;
                    for (int i = 0; i < phase; ++i) w.tick(1.0f / 30.0f);
                    w.destroy(vid);
                    tick(w, 8.0f);
                    const sim::Unit* v = w.unit(vid);
                    if (!v || v->alive()) {
                        ++survived;
                        if (worst.empty())
                            worst = "phase " + std::to_string(phase) +
                                    (v ? " hp=" + std::to_string(v->hp) : " gone");
                    }
                }
                check(survived == 0, "self-destruct inside a friendly heal aura still kills",
                      survived ? (std::to_string(survived) + "/30 survived, first at " + worst)
                               : (std::string("healer=") + healer->id + ", all 30 phases"));
            }

            // It LEAVES YOUR COMMAND rather than blowing up: no wreck behind it,
            // and nobody is credited with a kill. Retail's self-destruct mission
            // applies damage type 5, which is neither the explosion type (3, the
            // one that gibs) nor anything a weapon in the shipped data carries.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs;
                cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                cfg.slots[0].team = 0; cfg.slots[1].team = 1;
                sim::setupMatch(w, reg, cfg);
                const size_t feats0 = w.features().size();
                int id = w.spawn(regen, 500, 500, 0, 0);
                w.destroy(id);
                for (int i = 0; i < 30 * 8; ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                check(!u || !u->alive(), "the countdown does end the unit");
                check(w.features().size() == feats0,
                      "...and it leaves no wreck -- it fades, it does not die",
                      "features " + std::to_string(feats0) + " -> " +
                          std::to_string(w.features().size()));
            }

            // Pressing again while it counts down cancels, and the unit lives.
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int id = w.spawn(regen, 500, 500, 0, 0);
            w.destroy(id);
            tick(w, 1.0f);
            w.destroy(id);                     // cancel
            check(w.unit(id)->selfDestructT < 0.0f, "a second press cancels the countdown");
            tick(w, 8.0f);
            check(w.unit(id) && w.unit(id)->alive(), "and the cancelled unit survives");
        }
    }

    // ---- benchmark / stress placement --------------------------------------
    //
    // Both lay their armies on a blind lattice centred on each start spot, and
    // World::spawn does no terrain check, so a lattice point in a lake or on a cliff
    // used to put a unit there permanently -- it cannot walk out of a cell it could
    // not walk into. Measured on Ulasem with 8 players before the snap: 7.2% of a
    // top-intensity benchmark and 20.5% of a stress fill. That is not only ugly; it
    // quietly changes what the benchmark measures, since a fifth of the army is inert.
    //
    // A few stragglers are tolerable (a start spot can genuinely be hemmed in), so
    // this asserts a rate, not zero. It is set well below what the bug produced and
    // well above what the fix leaves, so it catches a regression without being
    // brittle about map details.
    std::printf("\n[benchmark / stress unit placement]\n");
    {
        auto misplaced = [&](sim::MatchConfig cfg) {
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots.resize(8);
            for (int i = 0; i < 8; ++i) {
                cfg.slots[size_t(i)].used = true;
                cfg.slots[size_t(i)].team = i;
                cfg.slots[size_t(i)].faction = i % 5;
            }
            sim::World w;
            sim::setupMatch(w, reg, cfg);
            int total = 0, bad = 0;
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type || u.type->canFly) continue;
                ++total;
                const int foot = std::clamp(std::max(u.type->footX, u.type->footZ), 1, 15);
                if (!w.navFor(u.type).fits(int(u.x) / 16, int(u.z) / 16, foot)) ++bad;
            }
            return std::pair<int, int>{bad, total};
        };
        {
            sim::MatchConfig cfg;
            cfg.stressTest = true;
            cfg.unitCap = 2000;
            auto [bad, total] = misplaced(cfg);
            check(total > 1000, "the stress fill actually spawned an army",
                  std::to_string(total) + " units");
            check(total && bad * 100 / total < 3,
                  "stress-test units start on terrain they can occupy",
                  std::to_string(bad) + "/" + std::to_string(total) + " misplaced");
        }
        {
            // Intensity 5 = the densest plan. setupMatch only BUILDS the plan, so tick
            // the 60s out to get the staged spawns on the map.
            sim::MatchConfig cfg;
            cfg.benchmark = 5;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots.resize(8);
            for (int i = 0; i < 8; ++i) {
                cfg.slots[size_t(i)].used = true;
                cfg.slots[size_t(i)].team = i;
                cfg.slots[size_t(i)].faction = i % 5;
            }
            sim::World w;
            sim::setupMatch(w, reg, cfg);
            for (int i = 0; i < 1800; ++i) w.tick(1.0f / 30.0f);
            int total = 0, bad = 0;
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type || u.type->canFly) continue;
                ++total;
                const int foot = std::clamp(std::max(u.type->footX, u.type->footZ), 1, 15);
                if (!w.navFor(u.type).fits(int(u.x) / 16, int(u.z) / 16, foot)) ++bad;
            }
            // Lower bar than the stress fill: this one has fought for 60s first, and
            // a smaller map kills faster (841 of the spawns survive on Inner Circle).
            check(total > 500, "the benchmark plan actually spawned an army",
                  std::to_string(total) + " units");
            check(total && bad * 100 / total < 3,
                  "benchmark units start on terrain they can occupy",
                  std::to_string(bad) + "/" + std::to_string(total) + " misplaced");
        }
    }

    // ---- per-category damage is a MULTIPLIER ------------------------------
    //
    // A DAMAGE entry other than `default` scales the base figure; it is not the
    // damage itself. Read as absolute it makes the units that declare one nearly
    // invulnerable, because the overrides are fractions: the Crusades Barracks
    // (arakeep, damagecategory=factory, 17162 HP, healtime=1.25) against a swordsman
    // declaring factory=0.5 took 0.5 damage a swing and regenerated 0.8 HP/s, so a
    // besieging army could not out-damage its own regen. Reported from a real game as
    // "barracks doesn't seem to be killable".
    std::printf("\n[per-category damage multiplier]\n");
    {
        // The contract itself, on values rather than on shipped data. Spot-checking a
        // real unit is fragile here for a reason worth recording: the first version of
        // this test used aradrag's dragon=2, which sits on WEAPON2 (Fire Ball) while
        // `type->weapon` is WEAPON1 (Fire Breath) -- so it failed against CORRECT code
        // and looked like a second bug. The scaling rule is what matters; the siege
        // below covers the shipped data.
        sim::UnitType tgt{};
        tgt.categories = {"factory", "aramon"};
        sim::Weapon w{};
        w.damage = 360;
        check(w.damageVs(&tgt) == 360.0f, "no override -> base damage");
        w.dmgVs["factory"] = 0.5f;
        check(w.damageVs(&tgt) == 180.0f,
              "a 0.5 override HALVES the base, it is not 0.5 damage",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs["factory"] = 2.0f;
        check(w.damageVs(&tgt) == 720.0f, "a 2.0 override doubles it",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs["factory"] = 0.0f;
        check(w.damageVs(&tgt) == 0.0f, "a 0 override still means immune",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs.clear();
        w.dmgVs["naval"] = 0.25f;      // a category this target does not carry
        check(w.damageVs(&tgt) == 360.0f, "an override for another category is ignored");
    }
    {
        // The reported case, end to end: the Crusades Barracks must die to a besieging
        // force. A behavioural check, because the arithmetic above would still pass if
        // regen quietly out-paced the result.
        sim::TypeRegistry cbreg;
        sim::setupRegistry(cbreg, vfs, /*crusades=*/true);
        const sim::UnitType* keep  = cbreg.find("arakeep");
        const sim::UnitType* sword = cbreg.find("arasword");
        if (!keep || !sword || sword->weapon.damage <= 0) {
            std::printf("  (no crusades data; skipped)\n");
        } else {
            check(sword->weapon.damageVs(keep) > 1.0f,
                  "a Crusades swordsman does real damage to a factory",
                  std::to_string(sword->weapon.damageVs(keep)) + " per hit");
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, cbreg, cfg);
            const float kx = 1200, kz = 1200;
            const int kid = w.spawn(keep, kx, kz, 0, 0);
            for (int i = 0; i < 12; ++i) {
                const int id = w.spawn(sword, kx + (i % 4) * 24 - 140,
                                       kz + (i / 4) * 24 - 40, 0, 1);
                w.attack(id, kid, false);
            }
            int diedAt = -1;
            for (int sec = 0; sec < 180 && diedAt < 0; ++sec) {
                tick(w, 1.0f);
                const sim::Unit* k = w.unit(kid);
                if (!k || !k->alive()) diedAt = sec + 1;
            }
            // Generous bound: the assertion is "a siege makes progress", not a balance
            // figure. Unfixed this survives indefinitely (17162 -> 16615 over 600s).
            check(diedAt > 0, "12 swordsmen can destroy a Barracks",
                  diedAt > 0 ? "died at t=" + std::to_string(diedAt) + "s"
                             : "SURVIVED 180s of siege");
        }
    }

    // ---- gods are summoned BY THE SIM ---------------------------------------
    //
    // This was the client's job: GameView::simStep polled godReady() and ran its own
    // summonGod() after ticking. The headless referee has no GameView, so it never
    // summoned anything -- from the first god onward the server's world held one
    // fewer unit than every client's and the hashes split for the rest of the match.
    // A desync arriving tens of minutes in, with nothing in the command stream to
    // explain it (observed at tick 39780).
    //
    // So the test is deliberately about WHERE the work happens, not whether a god
    // can appear: tick a bare World, with nobody driving it but tick() itself, and
    // require the god to show up anyway. Any future move back out of the sim fails
    // here regardless of how correct the relocated code is.
    std::printf("\n[gods are summoned by the sim, not the client]\n");
    {
        sim::World w;
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = kMap;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        cfg.slots[0].team = 0; cfg.slots[1].team = 1;
        cfg.slots[0].faction = 0; cfg.slots[1].faction = 1;
        cfg.slots[0].used = true; cfg.slots[1].used = true;   // else setupMatch skips them
        cfg.gods = true;
        sim::setupMatch(w, reg, cfg);
        check(w.godsEnabled(), "the match enabled gods");
        check(w.player(0).godType != nullptr,
              "setup resolved the player's god type (the sim cannot look it up)");

        const size_t before = w.units().size();
        // Fill the favour directly rather than waiting for priests to channel it:
        // the economy that FILLS it is not what broke.
        w.enableGods(0.0f);
        w.player(0).godFavor = 1e9f;
        check(w.godReady(0), "player 0 is ready for its god");

        tick(w, 0.2f);                       // nothing but World::tick runs here

        check(!w.godReady(0), "the readiness is consumed");
        check(w.player(0).godSummoned, "and the player is marked summoned");
        check(w.units().size() == before + 1,
              "ticking the world ALONE summoned the god",
              std::to_string(before) + " -> " + std::to_string(w.units().size()));
        bool isGod = false;
        for (const auto& u : w.units())
            if (u.alive() && u.player == 0 && u.type && u.type->id.size() > 3 &&
                u.type->id.substr(u.type->id.size() - 3) == "god") isGod = true;
        check(isGod, "and the new unit is player 0's god");
        // The other player, with no favour, must NOT have one -- otherwise a test
        // that counted units would pass on a sim that summoned indiscriminately.
        check(!w.player(1).godSummoned, "a player without favour gets nothing");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
