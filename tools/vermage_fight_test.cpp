// vermage_fight_test: a Peewee (armpw) on an attack-move with Maneuver fires
// and keeps firing at a Hold Position A.K. (corak). The test began as the
// Kingdoms Kirenna (VERMAGE) attack-move check -- before the retail target
// search (fx-tsearch step 6) she dealt no damage (docs/retail-engine.md,
// "Kirenna on an attack-move") -- and keeps its scenes on TA's front-line
// Kbots:
//
//   vermage_fight_test <retail-install-dir> [--legion] [--trace] [--pre N] [--dx PX]
//
// A fight-move leg is retail's MoveFight order, a Move with response mode -1
// (World::markFightLeg): its scan takes only an enemy the player sees,
// within the selected slot's range, so the unit stops on sight and the
// attack fires. Whether it then keeps firing depends on its script's aim
// handshake with its walk cycle at the stop; Legion clears a stalled aim
// (World::legionAimStall), Retail mode keeps retail's race. (The addresses
// these mechanisms were first mapped at are KINGDOMS.icd's.)
//
// Checks (flat map, shipped units, a hold-position A.K. of player 1):
//   - on the attack-move the Peewee takes the A.K. only once its player sees it;
//   - it fires and the A.K. loses hit points, and the A.K. hits it;
//   - Legion: it keeps firing, at least kSustained shots in the 60 s, here
//     and from every one of kPhases start phases (the order tick and its
//     start point varied). Retail mode is not checked for continued fire;
//   - on a plain move past it the Peewee fires (the sweep aims its automatic
//     slot as it walks).
// --trace prints its script state per tick of the attack-move; --pre and
// --dx move the start phase (ticks before the order, px further back).
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "tnt/tnt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ta::sim {
struct RetailReplayProbe {
    static const auto& scriptStatics(const World& world, int id) { return world.unitScripts_.at(id).peek().vm.statics; }
    // The scripts its threads are in (by entry), for --trace.
    static std::string threads(const World& world, const Unit& u) {
        const auto& vm = world.unitScripts_.at(u.id).peek().vm;
        const auto& file = *u.type->script();
        std::string out;
        for (const auto& t : vm.threads) {
            if (!t.words[0]) continue;
            const uint32_t pc = t.words[1];
            const std::string* name = nullptr; uint32_t best = 0;
            for (const auto& sc : file.scripts)
                if (sc.entry <= pc && sc.entry >= best) { best = sc.entry; name = &sc.name; }
            out += ' '; out += name ? *name : "?";
        }
        return out;
    }
};
}  // namespace ta::sim

using namespace ta;

namespace {
int failures = 0;
sim::PathfindingMode mode = sim::PathfindingMode::Retail;
bool trace = false;      // --trace: its script state per tick of the attack-move
constexpr float kTick = 1.f / 30;
constexpr int kSustained = 8;   // shots in 60 s that count as continued fire

void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++failures;
}

struct Run {
    int acquired = -1;      // first tick the front order was an auto attack on the A.K.
    bool seenAtAcquire = false;
    int shots = 0;          // ticks the Peewee fired
    long dealt = 0;         // hit points the A.K. lost
    int hits = 0;           // ticks the Peewee lost hit points
    float dist = 0;         // centre distance at the end
    bool standing = false;  // speed 0 at the end
    bool aiming = false;    // its selected slot's aim started (flags 0xe0) at the end
};

struct Phase {
    int pre = 30;      // ticks before the order
    float dx = 0;      // px further back than the default start
};

std::string describe(const Run& r) {
    char b[200];
    std::snprintf(b, sizeof b, "acquired %d (seen %d), shots %d, dealt %ld, hits taken %d, %.1f px off, standing %d, aiming %d",
                  r.acquired, int(r.seenAtAcquire), r.shots, r.dealt, r.hits, r.dist, int(r.standing), int(r.aiming));
    return b;
}

Run fight(const hpi::Vfs& vfs, const sim::TypeRegistry& reg, bool attackMove, Phase phase = {}) {
    tnt::Map map;
    map.width = map.height = 128;
    map.seaLevel = 0;
    map.heights.assign(size_t(128) * 128, 70);
    map.features.assign(size_t(128) * 128, 0xffff);
    sim::World w;
    w.setPathfindingMode(mode);
    w.setTerrain(map.heights, map.width, map.height, map.seaLevel, &map.features);
    sim::registerMapFeatures(w, map, vfs, &reg);
    w.setPathService(true);
    Run r;
    const sim::UnitType* shooterType = reg.find("armpw");
    const sim::UnitType* foeType = reg.find("corak");
    const sim::UnitType* spotterType = reg.find("armck");
    if (!shooterType || !foeType || !spotterType) { check(false, "shipped armpw, corak and armck load"); return r; }
    const float sx = 300, sz = 1000;
    const int foe = w.spawn(foeType, sx + 1100, sz, -1.5707963f, 1);
    w.unit(foe)->moveState = 0;
    w.unit(foe)->hp = sim::Fixed::fromInt(30000);
    const int me = w.spawn(shooterType, sx - phase.dx, sz, 1.5707963f, 0);
    w.unit(me)->moveState = 1;   // Maneuver
    // A second unit of the shooter's player, far off and passive: the sweep
    // skips a player's only unit.
    const int other = w.spawn(spotterType, 150, 1900, 0.f, 0);
    w.unit(other)->fireState = 0;
    w.unit(other)->moveState = 0;
    for (int i = 0; i < phase.pre; ++i) w.tick(kTick);
    if (attackMove) w.attackMove(me, sx + 1800, sz, false);
    else w.order(me, sx + 1800, sz, false);
    auto hp = w.unit(me)->hp;
    for (int i = 0; i < 1800; ++i) {
        w.tick(kTick);
        const sim::Unit* u = w.unit(me);
        const sim::Unit* t = w.unit(foe);
        if (!u || !t) break;
        if (r.acquired < 0 && !u->orders.empty() && u->orders.front().autoTarget &&
            u->orders.front().targetId == foe) {
            r.acquired = int(w.tickCount());
            r.seenAtAcquire = w.playerSees(0, *t);
        }
        r.shots += u->firedWeapons != 0;
        r.hits += u->hp < hp;
        // Both sides are kept alive: a Peewee (250 hp) would fall to the A.K.
        w.unit(me)->hp = hp = sim::Fixed::fromInt(u->type->maxHp);
        r.dealt += 30000 - t->hp.floorInt();
        w.unit(foe)->hp = sim::Fixed::fromInt(30000);
        r.dist = std::hypot((t->x - u->x).toFloat(), (t->z - u->z).toFloat());
        r.standing = u->speed.v == 0;
        r.aiming = (u->weaponAim[0].flags & 0xe0) != 0;
        if (trace && attackMove) {
            // Statics: 2 aiming, 3 moving (MoveWatcher), 5 TurnDirection,
            // 6 firing, 7 AimWeapon's gate, 9 restore_legs played, 10
            // restore_x played; the Move legs' mission stage and wake.
            const auto& st = sim::RetailReplayProbe::scriptStatics(w, me);
            std::string legs;
            for (const auto& o : u->orders)
                if (o.goal) {
                    char b[48];
                    std::snprintf(b, sizeof b, " [stage %d wake %d]", int(o.mission.stage), int(o.mission.deadline));
                    legs += b;
                }
            std::printf("    t%u x %.2f v %.2f order %d/%d fired %d aim %02x slot %d auto %d s2 %u s3 %u s5 %d s6 %u s7 %u "
                        "s9 %u s10 %u wait %d |%s |%s\n",
                        w.tickCount(), u->x.toFloat(), u->speed.toFloat(),
                        u->orders.empty() ? -1 : int(u->orders.front().autoTarget),
                        u->orders.empty() ? -1 : u->orders.front().targetId, int(u->firedWeapons),
                        unsigned(u->weaponAim[0].flags), u->slotTarget[0],
                        int(u->slotAuto), st[2], st[3], int(int32_t(st[5])), st[6], st[7], st[9], st[10],
                        int(u->aimWait[0]), sim::RetailReplayProbe::threads(w, *u).c_str(),
                        legs.c_str());
        }
    }
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vermage_fight_test <retail-install-dir> [--legion] [--trace] [--pre N] [--dx PX]\n");
        return 2;
    }
    Phase base;
    bool one = false;   // --pre/--dx: that phase only
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--legion") mode = sim::PathfindingMode::Legion;
        else if (a == "--trace") trace = one = true;
        else if (a == "--pre" && i + 1 < argc) { base.pre = std::atoi(argv[++i]); one = true; }
        else if (a == "--dx" && i + 1 < argc) { base.dx = float(std::atof(argv[++i])); one = true; }
    }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    sim::setupRegistry(reg,vfs);
    const bool legion = mode == sim::PathfindingMode::Legion;
    std::printf("[%s pathfinding]\n", legion ? "Legion" : "Retail");

    const Run am = fight(vfs, reg, true, base);
    std::printf("  attack-move: %s\n", describe(am).c_str());
    if (one) return 0;
    check(am.acquired >= 0 && am.seenAtAcquire, "the attack-move takes the A.K. only once its player sees it",
          describe(am));
    check(am.shots > 0 && am.dealt > 0, "attack_move_fires: the Peewee fires and the A.K. loses hit points",
          describe(am));
    // It stands inside the hold-position A.K.'s range: its sweep takes it.
    check(am.hits > 0, "the A.K. hits the Peewee", describe(am));
    if (legion) {
        // Continued fire is checked under Legion only. In Retail mode it is
        // the script race's side at the start phase, which any change to the
        // random draw order can move without a regression.
        check(am.shots >= kSustained && am.dealt >= 1000, "Legion: it keeps firing (here: 419 shots, 1605 damage)",
              describe(am));
        // The race's side is the phase of the walk cycle at the stop; Legion
        // clears a stalled aim in every one.
        constexpr int kPhases = 9;
        int sustained = 0;
        std::string worst;
        for (int n = 0; n < kPhases; ++n) {
            const Phase ph{30 + n % 3, float(24 * (n / 3))};
            const Run r = fight(vfs, reg, true, ph);
            if (r.shots >= kSustained) ++sustained;
            else if (worst.empty()) worst = "pre " + std::to_string(ph.pre) + " dx " + std::to_string(int(ph.dx)) + ": " + describe(r);
        }
        check(sustained == kPhases, "Legion: it keeps firing from every start phase (" + std::to_string(sustained) + " of " +
              std::to_string(kPhases) + ")", worst);
    }

    const Run mv = fight(vfs, reg, false);
    std::printf("  plain move: %s\n", describe(mv).c_str());
    check(mv.shots > 0, "on a plain move past it the sweep aims its automatic slot as it walks, and it fires",
          describe(mv));

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
