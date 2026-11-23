// retailgap_test: verifies the retail-parity sim fixes that have no other harness.
//
//   retailgap_test <retail-install-dir> [slice[,slice...]]
//
// The checks are grouped into slices (weapons, rules, fireatwill, orders) so ctest
// can run them as parallel tests; naming slices runs only those, none runs all.
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
#include <tuple>
#include <utility>
#include <vector>

using namespace tak;

namespace {
int failures = 0;

const char* const kSlices[] = {"weapons", "rules", "fireatwill", "orders"};
std::vector<std::string> selected;   // empty: every slice
bool want(const char* slice) {
    return selected.empty() || std::find(selected.begin(), selected.end(), slice) != selected.end();
}
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
    if (argc < 2) { std::fprintf(stderr, "usage: retailgap_test <retail-install-dir> [slice[,slice...]]\n"); return 2; }
    hpi::Vfs vfs = hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry reg;
    reg.loadMoveInfo(vfs, "gamedata/moveinfo.tdf");
    reg.loadDir(vfs, "units/");
    if(argc==3 && std::string(argv[2])=="--blood-palette") {
        const auto* hunter=reg.find("zonhunt");
        check(hunter && hunter->bloodColors==std::array<uint32_t,3>{0xffa02300,0xffaa2800,0xffb41e05},
              "Hunter retains all three authored blood colors");
        return failures?1:0;
    }
    if (argc >= 3) {
        const std::string list = argv[2];
        for (size_t at = 0; at <= list.size();) {
            const size_t comma = std::min(list.find(',', at), list.size());
            const std::string name = list.substr(at, comma - at);
            if (std::find(std::begin(kSlices), std::end(kSlices), name) == std::end(kSlices)) {
                std::fprintf(stderr, "retailgap_test: unknown slice '%s'\n", name.c_str());
                return 2;
            }
            selected.push_back(name);
            at = comma + 1;
        }
    }

    // ---- 1. [EXPLODEAS] ----------------------------------------------------
    if (want("weapons")) {
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
            float aHp0 = w.unit(aId)->hp.toFloat(), farHp0 = w.unit(farId)->hp.toFloat();
            check(aHp0 > 0 && farHp0 > 0, "victims spawned alive");

            w.unit(ratId)->hp = sim::Fixed();    // the rat dies -> its EXPLODEAS should fire
            tick(w, 0.2f);

            const sim::Unit* a = w.unit(aId);
            const sim::Unit* b = w.unit(bId);
            const sim::Unit* far = w.unit(farId);
            check(a && (!a->alive() || a->hp.toFloat() < aHp0), "adjacent enemy took the blast");
            check(b && (!b->alive() || b->hp.toFloat() < aHp0), "second adjacent enemy took the blast");
            check(far && far->alive() && far->hp.toFloat() >= farHp0, "distant enemy untouched");

            // Stone/frozen packets suppress death weapons; self-destruction
            // selects a separate weapon block, absent from the shipped rat.
            for(int special:{14,15,5}) {
                sim::World quiet;
                sim::setupMatch(quiet,reg,cfg);
                const int source=quiet.spawn(rat,500,500,0,0);
                const int neighbor=quiet.spawn(victim,520,500,0,1);
                auto* dying=quiet.unit(source);
                dying->deathType=special;
                dying->stonedFor=special==14 ? 100 : 0;
                dying->frozenFor=special==15 ? 100 : 0;
                dying->hp=sim::Fixed();
                const auto hp=quiet.unit(neighbor)->hp;
                // The death weapon fires at retirement (512ae0), once Dying is over:
                // watch every tick until then.
                bool hit=false;
                for(int t=0;t<600;++t) {
                    quiet.tick(1.f/30);
                    hit=hit || !quiet.hits().empty();
                    if(!quiet.unit(source)->deathPending)break;
                }
                check(!quiet.unit(source)->alive() && !quiet.unit(source)->deathPending,
                      "special death control reached the death transition");
                check(!hit && quiet.unit(neighbor)->hp==hp,
                      special==14 ? "stone death emits no death blast or damage" :
                      special==15 ? "frozen death emits no death blast or damage" :
                                    "self-destruct does not reuse the ordinary death weapon");
            }

            // An explicit self-destruct weapon must retain its own art and damage.
            sim::UnitType custom=*rat;
            custom.hasSelfDestructAs=true;
            custom.selfDestructAs=rat->explodeAs;
            custom.selfDestructAs.damage=100;
            custom.selfDestructAs.explosionClass="small explosion";
            sim::World explicitWeapon;
            sim::setupMatch(explicitWeapon,reg,cfg);
            const int source=explicitWeapon.spawn(&custom,500,500,0,0);
            const int neighbor=explicitWeapon.spawn(victim,520,500,0,1);
            const auto hp=explicitWeapon.unit(neighbor)->hp;
            explicitWeapon.unit(source)->deathType=sim::Unit::kDeathSelfDestruct;
            explicitWeapon.unit(source)->hp=sim::Fixed();
            // It bursts at retirement, when its Dying is over (512ae0).
            bool published=false;
            for(int t=0;t<600 && !published;++t) {
                explicitWeapon.tick(1.f/30);
                published=std::any_of(explicitWeapon.hits().begin(),explicitWeapon.hits().end(),
                                      [&](const auto& hit){return hit.weapon==&custom.selfDestructAs;});
            }
            check(published,"self-destruct publishes its own authored weapon effect");
            check(explicitWeapon.unit(neighbor)->alive() && explicitWeapon.unit(neighbor)->hp<hp &&
                  explicitWeapon.unit(neighbor)->hp>hp-sim::Fixed::fromInt(150),
                  "self-destruct uses its separate low damage, not the ordinary lethal blast");
        }
    }

    // ---- 1b. weapon-class parse ------------------------------------------
    if (want("weapons")) {
        std::printf("[weapon class parse]\n");
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
        if (const sim::UnitType* t = reg.find("verbal"); t && !t->weapons.empty()) {
            const sim::Weapon& bolt = t->weapons[0];
            check(bolt.shotModel == "verbal1", "Ballista bolt parses its regular projectile mesh");
            check(bolt.veteranShotModel == "verbal1_vet" && bolt.veteranLevel == 10,
                  "Ballista bolt parses veteranmodel/veteranlevel from WEAPON1");
            check(!bolt.usesVeteranShotModel(9) && bolt.usesVeteranShotModel(10) &&
                  bolt.usesVeteranShotModel(11),
                  "Ballista switches its per-shot mesh at rank 10");

            const sim::UnitType* victim = reg.find("arasword");   // as in [EXPLODEAS]
            auto firedVariant = [&](int rank) {
                sim::World w;
                sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=kMap;
                cfg.slots={sim::MatchSlot{},sim::MatchSlot{}};
                cfg.slots[0].team=0;cfg.slots[1].team=1;
                sim::setupMatch(w,reg,cfg);
                float sx=1200,sz=1000,tx=1200,tz=1500;
                legalSpot(w,t,sx,sz);legalSpot(w,victim,tx,tz);
                const int shooter=w.spawn(t,sx,sz,0,0);
                const int targetId=w.spawn(victim,tx,tz,0,1);
                if(auto* unit=w.unit(shooter)) {unit->veteran=rank;unit->stance=0;}
                w.attack(shooter,targetId,false);
                for(int step=0;step<300 && w.projectiles().empty();++step)
                    w.tick(1.0f/30.0f);
                if(w.projectiles().empty())return -1;
                return int(w.projectiles().front().projectileUsesVeteranModel);
            };
            check(firedVariant(9)==0 && firedVariant(10)==1,
                  "Ballista shots snapshot their rank-selected projectile mesh at launch");
        }
    }

    // ---- 2. totalallowed ---------------------------------------------------
    if (want("weapons")) {
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
            if (auto* d = w.unit(d1)) d->hp = sim::Fixed();   // it dies
            tick(w, 0.2f);
            check(!w.atTypeCap(0, drag), "dragon died -> the slot frees again");
            const sim::UnitType* plain = reg.find("arasword");
            if (plain) check(!w.atTypeCap(0, plain), "an uncapped type is never capped");
        }
    }

    // ---- 2b. guided homing --------------------------------------------------
    if (want("weapons")) {
        std::printf("[guided homing]\n");
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
            sim::World w, peer;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            sim::setupMatch(peer, reg, cfg);
            // Homing needs an unobstructed firing lane, independent of map trees.
            w.setTerrain(std::vector<uint8_t>(256 * 256, 100), 256, 256, 20);
            peer.setTerrain(std::vector<uint8_t>(256 * 256, 100), 256, 256, 20);
            int shooter = w.spawn(demon, 1000, 1000, 0, 0);
            int mover = w.spawn(prey, 1000, 1600, 0, 1);   // 600px away, in range
            int peerShooter = peer.spawn(demon, 1000, 1000, 0, 0);
            int peerMover = peer.spawn(prey, 1000, 1600, 0, 1);
            if (auto* su = w.unit(shooter)) su->stance = 0;
            if (auto* su = peer.unit(peerShooter)) su->stance = 0;
            w.attack(shooter, mover, false);
            peer.attack(peerShooter, peerMover, false);
            // Tick until a shot is in the air.
            int guard = 0;
            bool paired=true;
            while (w.projectiles().empty() && guard++ < 400) {
                w.tick(1.0f / 30.0f);peer.tick(1.0f / 30.0f);
                paired=paired && w.stateHash()==peer.stateHash();
            }
            check(!w.projectiles().empty(), "the demon launched a guided shot");
            check(paired && w.stateHash()==peer.stateHash(),
                  "paired World instances retain the same Guided state hash at launch");
            if (!w.projectiles().empty()) {
                const auto& shot=w.projectiles().front();
                check(shot.guided3d && shot.targetId==mover && shot.fromId==shooter && shot.slot==0,
                      "Guided launch stores its owner, slot, target, and native XYZ trajectory");
                check(shot.position==shot.muzzle && shot.x.v==shot.position[0] &&
                      shot.z.v==shot.position[2] && shot.start==w.tickCount()+1 && shot.end>shot.start,
                      "Guided launch keeps the script muzzle, range lifetime, and delayed start");
                // Jump the target 300px sideways: a straight shot now misses by far
                // more than the hit radius.
                if (auto* m = w.unit(mover)) { m->x = tak::sim::Fixed::fromInt(1300); m->orders.clear(); }
                if (auto* m = peer.unit(peerMover)) { m->x = tak::sim::Fixed::fromInt(1300); m->orders.clear(); }
                float hp0 = w.unit(mover)->hp.toFloat();
                for (int i = 0; i < 90; ++i) {
                    w.tick(1.0f / 30.0f);peer.tick(1.0f / 30.0f);   // 3s < reload
                    paired=paired && w.stateHash()==peer.stateHash();
                }
                const sim::Unit* m = w.unit(mover);
                bool hurt = m && (!m->alive() || m->hp.toFloat() < hp0);
                check(hurt, "the shot curved onto the dodging target",
                      hurt ? "" : "untouched -- homing did not engage");
                check(paired && w.stateHash()==peer.stateHash(),
                      "moving ground-target guidance stays lockstep across paired Worlds");
            }
        }
    }

    // ---- 2b.1 guided XYZ against an airborne target and target loss --------
    if (want("weapons")) {
        const sim::UnitType* flyerBase=reg.find("zonhunt");
        const sim::UnitType* demon=reg.find("tarfire");
        if(!flyerBase || !flyerBase->canFly || !demon || demon->weapons.empty()) {
            std::printf("  (missing guided shooter or flying target; skipped)\n");
        } else {
            auto harmlessFlyer=*flyerBase;
            harmlessFlyer.weapons.clear();harmlessFlyer.weapon.damage=0;
            // Unit HP is retail 16.16 Fixed (max < 32768); 100000 wraps at
            // spawn and makes the target dead before the first aim query.
            harmlessFlyer.maxHp=30000;harmlessFlyer.healTime=0;
            harmlessFlyer.categories.clear();harmlessFlyer.catIds.clear();
            check(demon->weapons[0].kind==sim::Weapon::Kind::Guided &&
                  demon->weapons[0].flameKind<0 && !demon->weapons[0].beam &&
                  !demon->weapons[0].noAir && demon->weapons[0].damage>0,
                  "Fire Ball of Gloom is a projectile-backed Guided weapon that can hit flyers");
            sim::World w,peer;
            sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=kMap;
            cfg.slots={sim::MatchSlot{},sim::MatchSlot{}};
            cfg.slots[0].team=0;cfg.slots[1].team=1;
            sim::setupMatch(w,reg,cfg);
            sim::setupMatch(peer,reg,cfg);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            peer.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            const int shooter=w.spawn(demon,1000,1000,0,0);
            const int target=w.spawn(&harmlessFlyer,1000,1600,0,1);
            const int peerShooter=peer.spawn(demon,1000,1000,0,0);
            const int peerTarget=peer.spawn(&harmlessFlyer,1000,1600,0,1);
            if(auto* u=w.unit(shooter))u->stance=0;
            if(auto* u=peer.unit(peerShooter))u->stance=0;
            for(auto [world,id]:{std::pair{&w,target},std::pair{&peer,peerTarget}}) {
                if(auto* u=world->unit(id)) {
                    u->flightGroundMode=2;
                    u->flightY=sim::Fixed::fromInt(320);
                }
                // An explicit flight mission prevents idle VTOL standby from
                // beginning a landing during the attacker's aim/build-up ticks.
                world->order(id,1080,1600,false);
            }
            w.attack(shooter,target,false);
            peer.attack(peerShooter,peerTarget,false);
            const auto launchAim=w.queryWeaponAim(shooter,target,0);
            const auto peerLaunchAim=peer.queryWeaponAim(peerShooter,peerTarget,0);
            check(launchAim.has_value() && peerLaunchAim.has_value() &&
                  launchAim->heading==peerLaunchAim->heading && launchAim->pitch==peerLaunchAim->pitch &&
                  launchAim->target==peerLaunchAim->target,
                  "paired Fire Demons compute the same authored aim solution for the flying target");
            int guard=0;
            bool paired=w.stateHash()==peer.stateHash(),sawFireEvent=false;
            int fireEventTick=0;
            while(w.projectiles().empty() && guard++<400) {
                w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                paired=paired && w.stateHash()==peer.stateHash();
                const auto* state=w.unit(shooter);
                if(state && state->firedWeapons) {
                    sawFireEvent=true;fireEventTick=guard;
                    if(w.projectiles().empty())break;
                }
            }
            check(sawFireEvent,"Fire Demon releases its Guided weapon against the airborne target");
            if(w.projectiles().empty()) {
                const auto* state=w.unit(shooter);
                std::printf("  (guided air launch diagnostic: aim=%d heading=%u pitch=%u targetOrder=%d scriptTarget=%d fireTick=%d fired=%u reload=%d targetY=%d)\n",
                    int(launchAim.has_value()),launchAim?launchAim->heading:0,launchAim?launchAim->pitch:0,
                    state && !state->orders.empty()?state->orders.front().targetId:0,
                    state?state->scriptAimTarget:0,fireEventTick,state?state->firedWeapons:0,
                    state?state->reloads[0]:-1,w.unit(target)?w.unit(target)->flightY.v:0);
            }
            check(paired && w.stateHash()==peer.stateHash(),
                  "paired Worlds stay in lockstep while a Guided weapon aims at a flyer");
            const auto* airborneAtLaunch=w.unit(target);
            check(airborneAtLaunch && airborneAtLaunch->flightGroundMode==2,
                  "the flying target stays under active flight control through launch");
            check(!w.projectiles().empty() && w.projectiles().front().guided3d,
                  "a Guided shot launches toward a flying target with full XYZ state");
            bool trackedHeight=false;
            std::array<int32_t,3> lastVelocity{};
            if(!w.projectiles().empty()) {
                lastVelocity=w.projectiles().front().velocity;
                trackedHeight=lastVelocity[1]!=0;
                for(int i=0;i<6 && !w.projectiles().empty();++i) {
                    if(auto* u=w.unit(target)) {
                        u->x+=sim::Fixed::fromInt(8);
                        u->flightY+=sim::Fixed::fromInt(4);
                    }
                    if(auto* u=peer.unit(peerTarget)) {
                        u->x+=sim::Fixed::fromInt(8);
                        u->flightY+=sim::Fixed::fromInt(4);
                    }
                    w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                    paired=paired && w.stateHash()==peer.stateHash();
                    if(!w.projectiles().empty()) {
                        const auto& shot=w.projectiles().front();
                        trackedHeight=trackedHeight || shot.velocity[1]!=lastVelocity[1] ||
                            shot.position[1]!=shot.muzzle[1];
                        lastVelocity=shot.velocity;
                    }
                }
            }
            check(trackedHeight,"flying-target height changes the Guided projectile's simulated Y/velocity");
            bool lostTargetContinues=false;
            if(auto* u=w.unit(target)) {u->hp=sim::Fixed();u->deadFor=0;}
            if(auto* u=peer.unit(peerTarget)) {u->hp=sim::Fixed();u->deadFor=0;}
            if(!w.projectiles().empty()) {
                const auto velocity=w.projectiles().front().velocity;
                const auto position=w.projectiles().front().position;
                for(int i=0;i<3;++i) {
                    w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                    paired=paired && w.stateHash()==peer.stateHash();
                }
                if(!w.projectiles().empty()) {
                    const auto& shot=w.projectiles().front();
                    lostTargetContinues=shot.guided3d && shot.targetId==target && shot.velocity==velocity &&
                        shot.position!=position;
                }
            }
            check(lostTargetContinues,"losing the target leaves a Guided shot on its existing XYZ trajectory");
            check(paired && w.stateHash()==peer.stateHash(),
                  "flying-target guidance and loss remain lockstep across paired Worlds");
        }
    }

    // ---- 2b.2 guided homing on a carried target -------------------------------
    if (want("weapons")) {
        // 52e030 tests only +0x78's alive (0x1000000) and dying (0x1000) bits;
        // the attach (51b5a0) keeps both, so a target that boards a transport
        // is still homed on, at its SweetSpot over the host's attach point
        // (tools/re/probe_guided_projectile.py, carried mode). World passed the
        // shot's own position for an embarked target and flew it straight on.
        const sim::UnitType* demon=reg.find("tarfire");
        const sim::UnitType* prey=reg.find("arasword");
        const sim::UnitType* roc=reg.find("zonroc");
        if(!demon || !prey || !roc || demon->weapons.empty()) {
            std::printf("  (missing guided shooter, target or transport; skipped)\n");
        } else {
            sim::World w,peer;
            sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=kMap;
            cfg.slots={sim::MatchSlot{},sim::MatchSlot{}};
            cfg.slots[0].team=0;cfg.slots[1].team=1;
            sim::setupMatch(w,reg,cfg);
            sim::setupMatch(peer,reg,cfg);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            peer.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            int target=0,peerTarget=0,host=0,peerHost=0;
            for(auto [world,victim,carrier]:{std::tuple{&w,&target,&host},std::tuple{&peer,&peerTarget,&peerHost}}) {
                const int shooter=world->spawn(demon,1000,1000,0,0);
                *victim=world->spawn(prey,1000,1600,0,1);
                *carrier=world->spawn(roc,1500,1600,0,1);
                if(auto* u=world->unit(shooter))u->stance=0;
                world->attack(shooter,*victim,false);
            }
            int guard=0;
            bool paired=true;
            while(w.projectiles().empty() && guard++<400) {
                w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                paired=paired && w.stateHash()==peer.stateHash();
            }
            check(!w.projectiles().empty() && w.projectiles().front().guided3d,
                  "the demon launched a guided shot at the soon-carried target");
            if(!w.projectiles().empty()) {
                const auto launch=w.projectiles().front().velocity;
                const bool boarded=w.embarkInto(target,host) && peer.embarkInto(peerTarget,peerHost);
                check(boarded,"the target boards the Roc while the shot is in the air");
                bool homed=false;
                for(int i=0;i<6 && !w.projectiles().empty();++i) {
                    w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                    paired=paired && w.stateHash()==peer.stateHash();
                    const auto* cargo=w.unit(target);
                    if(!w.projectiles().empty() && cargo && cargo->embarked())
                        homed=homed || w.projectiles().front().velocity[0]>launch[0];
                }
                check(homed,"a Guided shot keeps homing on a target carried off (toward its host)",
                      homed ? "" : "flew straight on: the carried target was dropped");
            }
            check(paired && w.stateHash()==peer.stateHash(),
                  "carried-target guidance stays lockstep across paired Worlds");
        }
    }

    // ---- 2c. remote effect / wandering / mind control ------------------------
    if (want("rules")) {
        std::printf("[remote effect + wandering + mind control]\n");
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
                float hp0 = w.unit(victim)->hp.toFloat();
                for (int i = 0; i < 600; ++i) {
                    w.tick(1.0f / 30.0f);
                    if (const sim::Unit* v = w.unit(victim); v && (!v->alive() || v->hp.toFloat() < hp0)) break;
                }
                const sim::Unit* v = w.unit(victim);
                check(v && (!v->alive() || v->hp.toFloat() < hp0), "the remote effect landed on its target");
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
                int pulses = 0; float last = w.unit(victim)->hp.toFloat(); bool died = false;
                for (int i = 0; i < int(seconds * 30); ++i) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* v = w.unit(victim);
                    if (!v || !v->alive()) { died = true; break; }
                    if (v->hp.toFloat() < last - 0.01f) { ++pulses; last = v->hp.toFloat(); }
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
            // This checks storm damage, not navigation/LoS through Inner Circle.
            // Place the caster and victims on open terrain within firing range.
            w.setTerrain(std::vector<uint8_t>(160 * 160, 100), 160, 160, 20);
            w.buildNavClasses(reg);
            float wx = 1000, wz = 1000;
            legalSpot(w, witch, wx, wz);
            int caster = w.spawn(witch, wx, wz, 0, 0);
            if (auto* c = w.unit(caster)) c->mana = c->type->maxMana;
            // A dense field relative to the actual legal caster position (which
            // can differ from the requested point on this terrain), across the
            // whole region a 9-second tornado can reach
            // (it drifts ~400px forward and wanders hundreds of px sideways), so the
            // check does not depend on the exact RNG path.
            std::vector<int> crowd;
            for (int gz = 0; gz < 11; ++gz)
                for (int gx = 0; gx < 11; ++gx)
                    crowd.push_back(w.spawn(prey, wx - 100.0f + gx * 45.0f,
                                            wz + 100.0f + gz * 45.0f, 0, 1));
            float total0 = 0;
            for (int id : crowd) if (auto* u = w.unit(id)) total0 += u->hp.toFloat();
            for (int id : crowd) w.unit(id)->fireState = 0;
            w.attack(caster, crowd[2], false);
            int ticksWithDamage = 0;
            size_t maxStorms = 0;
            float last = total0;
            for (int i = 0; i < 600; ++i) {
                w.tick(1.0f / 30.0f);
                maxStorms = std::max(maxStorms, w.storms().size());
                float now = 0;
                for (int id : crowd) if (const sim::Unit* u = w.unit(id); u && u->alive()) now += u->hp.toFloat();
                if (now < last - 0.01f) ++ticksWithDamage;
                last = now;
            }
            check(last < total0, "the tornado chewed through the crowd it crossed",
                  "lost " + std::to_string(int(total0 - last)) + " HP; storms=" +
                  std::to_string(maxStorms) + " caster=" + std::to_string(wx) + "," + std::to_string(wz));
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
    if (want("rules")) {
        std::printf("[dropped bombs]\n");
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
            float hp0 = w.unit(v)->hp.toFloat();
            bool hurt = false; float closest = 1e9f;
            for (int i = 0; i < 1800; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* bb = w.unit(b);
                const sim::Unit* vv = w.unit(v);
                if (!vv) break;
                if (bb && vv) {
                    float d = std::sqrt((bb->x.toFloat() - vv->x.toFloat()) * (bb->x.toFloat() - vv->x.toFloat()) +
                                        (bb->z.toFloat() - vv->z.toFloat()) * (bb->z.toFloat() - vv->z.toFloat()));
                    closest = std::min(closest, d);
                }
                if (!vv->alive() || vv->hp.toFloat() < hp0) { hurt = true; break; }
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
    if (want("rules")) {
        std::printf("[auto weapon selection]\n");
        // Elsin carries Lightning (250px, free, 950), Meteor (400px, 200 mana,
        // 2000) and Earthen Wave (120px, 900 mana, 5000). The sim used to fire slot
        // 0 forever, so he cast Lightning all game and never spent a point of mana.
        const sim::UnitType* king = reg.find("araking");
        const sim::UnitType* prey = reg.find("arasword");
        if (!king || !prey || king->weapons.size() < 3) std::printf("  (missing defs; skipped)\n");
        else {
            sim::World selection;
            const int selected = selection.spawn(king, 1000, 1000, 0, 0);
            auto* selectedUnit = selection.unit(selected);
            const auto expectedSwitch = uint16_t(int32_t(king->weapons[1].reload*30.0));
            check(king->weapons[1].switchReloadTicks == expectedSwitch,
                  "Elsin's switch reload defaults to the authored reloadtime");
            selection.setWeapon(selected,1);
            check(selectedUnit->weaponSlot == 1 && selectedUnit->reloads[1] == expectedSwitch,
                  "changing weapon starts its retail switch reload");
            selectedUnit->reloads[1] = 7;
            selection.setWeapon(selected,1);
            selection.setWeapon(selected,-1);
            selection.setWeapon(selected,3);
            check(selectedUnit->weaponSlot == 1 && selectedUnit->reloads[1] == 7,
                  "repeated and invalid selections preserve the selected weapon countdown");
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
            bool eventsMatch=true;
            float prev[3] = {0, 0, 0};
            for (int i = 0; i < 900; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* tu = w.unit(t);
                if (!tu) break;
                for (size_t sl = 0; sl < tower->weapons.size() && sl < 3; ++sl) {
                    const bool fired=tu->reloads[sl] > prev[sl] + 0.01f;
                    if (tu->firedWeapons & (1u<<sl)) used[sl] = true;
                    eventsMatch=eventsMatch && bool(tu->fireAnimations & (1u<<sl))==fired;
                    prev[sl] = tu->reloads[sl];
                }
            }
            int n = 0; for (bool b2 : used) if (b2) ++n;
            check(eventsMatch, "FireWeapon animation slots match reload starts independently of projectile timing");
            check(n > 1, "araat fires BOTH of its weapons independently",
                  std::to_string(n) + " weapons fired");
        }
    }

    // ---- 3. data-only mission + InitialMission ------------------------------
    if (want("rules")) {
        std::printf("[data-only mission (.cob optional) + InitialMission]\n");
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
    if (want("rules")) {
        std::printf("[campaign win/lose conditions]\n");
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
        // takmission28_dh is an escort: NPCDERN must reach within two cells of Z=11 (victory) and losing
        // every Dern is defeat. Neither condition was parsed, so it could not be
        // won OR lost. (It is also one of the 11 missions that ship no .cob.)
        const sim::UnitType* dern = reg.find("npcdern");
        {
            sim::World w;
            if (runMission("takmission28_dh", w) && dern) {
                tickM(w, 2.0f);
                check(w.missionOutcome() == 0, "escort mission does not resolve at the start");
                for (auto& u : w.units())
                    if (u.type == dern) if (auto* p = w.unit(u.id)) p->hp = sim::Fixed();
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
                        if (auto* p = w.unit(u.id)) { p->z = tak::sim::Fixed::fromInt(11 * 16); ++moved; }
                tickM(w, 2.0f);
                check(moved > 0 && w.missionOutcome() > 0,
                      "getting it within the native two-cell line tolerance is a VICTORY");
            }
        }
        // takmission11_dh: the human plays Taros and must keep TARPRIE2 alive.
        {
            sim::World w;
            const sim::UnitType* pr = reg.find("tarprie2");
            if (runMission("takmission11_dh", w) && pr) {
                tickM(w, 2.0f);   // let the condition arm (it must not fire at t=0)
                for (auto& u : w.units())
                    if (u.type == pr) if (auto* p = w.unit(u.id)) p->hp = sim::Fixed();
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
        // GET's player argument is retail's 0-based record index (4d4060, the
        // index Create takes): takmission04's GET(5, 1) counts Player2, the
        // Veruna opponent. Until gap hunt 12 it counted Player1, the human, so
        // losing your own army won the mission and beating Veruna did not.
        for (bool ownSide : {true, false}) {
            sim::World w;
            int human = 0;
            if (!sim::setupMission(w, reg, vfs, "takmission04_ph", human)) continue;
            tickM(w, 1.0f);
            int enemy = -1;
            for (const auto& u : w.units())
                if (u.alive() && u.player != human && !w.player(u.player).passiveNeutral) enemy = u.player;
            const int side = ownSide ? human : enemy;
            int left = 0;
            for (const auto& u : w.units()) if (u.alive() && u.player == side) ++left;
            for (auto& u : w.units())
                if (u.alive() && u.player == side && left > 3)
                    if (auto* p = w.unit(u.id)) { p->hp = sim::Fixed(); --left; }
            tickM(w, 5.0f);
            if (ownSide)
                check(enemy >= 0 && w.missionOutcome() != 1, "takmission04: losing your own army is no victory",
                      "outcome=" + std::to_string(w.missionOutcome()));
            else
                check(enemy >= 0 && w.missionOutcome() == 1, "takmission04: Veruna down to three units is the victory",
                      "outcome=" + std::to_string(w.missionOutcome()));
        }
    }

    // --- Random Start Locations -------------------------------------------
    // Retail's room option. Two requirements: the same seed must reproduce the
    // same arrangement on every peer (lockstep), and a different seed must
    // actually move somebody (otherwise the option does nothing).
    if (want("rules")) {
        std::printf("[random start locations]\n");
        auto startsFor = [&](bool rnd, uint32_t seed) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.startMode = rnd ? sim::StartMode::Random : sim::StartMode::Fixed;
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
    if (want("rules")) {
        std::printf("[cosmetic lows: FBI fields reach the registry]\n");
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
            check(!(wall->maxVel > sim::Fixed()) || !wall->noShadow, "a wall is a structure anyway");
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
    if (want("rules")) {
        std::printf("[AdjustJoy repair aura]\n");
        const sim::UnitType* pri = reg.find("arapries");
        const sim::UnitType* vic = reg.find("arasword");
        if (pri && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            // No Monarch here, so the pool starts empty (retail): give it some.
            w.player(0).mana = 2800;
            int pid = w.spawn(pri, 600, 600, 0, 0);
            int vid = w.spawn(vic, 660, 600, 0, 0);   // well inside radius 200
            (void)pid;
            sim::Unit* v = w.unit(vid);
            v->hp = sim::Fixed::fromFloat(v->type->maxHp * 0.25f);
            float hp0 = v->hp.toFloat();
            float mana0 = w.player(0).mana;
            for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);   // 2s = two 1Hz pulses
            float hp1 = w.unit(vid)->hp.toFloat();
            check(hp1 > hp0, "a damaged ally next to an Acolyte is repaired",
                  std::to_string(int(hp0)) + " -> " + std::to_string(int(hp1)));
            check(w.player(0).mana < mana0, "and the Acolyte's owner pays the mana",
                  std::to_string(int(mana0)) + " -> " + std::to_string(int(w.player(0).mana)));
            // An undamaged unit must not be touched (and must still count toward N).
            int fid = w.spawn(vic, 640, 620, 0, 0);
            float full0 = w.unit(fid)->hp.toFloat();
            for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);
            check(w.unit(fid)->hp.toFloat() <= full0 + 0.01f, "an undamaged ally is left alone");
        }
    }

    // The turn/brake coupling replaced a flat "big turn -> 30% speed" cliff with
    // retail's arc-distance rule. The risk is a unit that brakes forever and never
    // arrives, so test arrival on a straight run AND on a right-angle dogleg.
    if (want("rules")) {
        std::printf("[turn/brake coupling]\n");
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
                float dx = u->x.toFloat() - 900.0f, dz = u->z.toFloat() - sz;
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
    if (want("rules")) {
        std::printf("[ballistic: fields + lobbing]\n");
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
        // The 2D clear-line gate is decided per weapon: every plain Ballistic
        // shell is exempt (its 3D arc decides, as in retail), lobbed or not;
        // a shot that flies the line is not.
        if (const sim::Weapon* w = wpn("vermort", 0))
            check(!w->needsClearLine(), "the Veruna mortar's shell needs no 2D line");
        if (const sim::Weapon* w = wpn("aratre", 0))
            check(w->ballisticClass() && !w->lobPreferred && !w->needsClearLine(),
                  "nor does the Trebuchet's (Ballistic, not lobpreferred)");
        if (const sim::Weapon* w = wpn("arabow", 0))
            check(!w->needsClearLine(), "nor an archer's arrow (Ballistic)");
        if (const sim::Weapon* w = wpn("arasiege", 1))
            check(w->straight && w->needsClearLine(), "a line-of-sight shot does");
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
    if (want("rules")) {
        std::printf("[walls block, all four factions]\n");
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
            float wx = float(cx) * 16 + t->footX * 8, wz = float(cz) * 16 + t->footZ * 8;
            bool before = w.nav().walkable(cx, cz);
            w.spawn(t, wx, wz, 0, 0);
            sim::blockFootprint(w.nav(), *t, wx, wz, true);
            bool after = w.nav().walkable(cx, cz);
            check(before && !after, "the wall blocks its footprint",
                  std::string(id) + " (canmove=" + (t->canMove ? "1" : "0") + ")");
        }
    }
    if (want("rules")) {
        std::printf("[a missed shell craters]\n");
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
            // A body apart from the mark (a footprint is 32px): bodies spawned
            // inside each other share the body plane's cells, and the splash
            // reads one occupant per cell. This shell lands at about (911,873),
            // so the friendly stands on that side of the aim point.
            int bystander = w.spawn(vic, 932, 900, 0, 1);   // beside the aim point
            int friendly = w.spawn(vic, 900, 868, 0, 0);    // retail blast also hits allies
            // The splashed swordsmen see each other: on Fire At Will they
            // fight, and their own blows draw at the same site. Passive, they
            // hold fire, so every draw here is the shell's.
            for (int id : {mark, bystander, friendly}) w.setStance(id, 2);
            // A unit killed outright may be gone from the table.
            auto hpOf = [&](int id) { const sim::Unit* u = w.unit(id); return u ? u->hp.toFloat() : -1.0e9f; };
            float by0 = hpOf(bystander);
            float friendly0 = hpOf(friendly);
            int damageSpreadDraws = 0;
            bool perRecipientDrawsMatch = true;
            int spreadDamageTicks = 0;
            w.setCrtRngObserver([&](const sim::World::RngObservation& row) {
                if (row.retailReturnAddress == 0x52a3ba) ++damageSpreadDraws;
            });
            w.attack(gun, mark, false);
            bool flew = false, moved = false;
            float previousTargetHp[3]{hpOf(mark), by0, friendly0};
            for (int i = 0; i < 30 * 20; ++i) {
                const int drawsBefore = damageSpreadDraws;
                w.tick(1.0f / 30.0f);
                const int drawsThisTick = damageSpreadDraws - drawsBefore;
                int damagedRecipients = 0;
                const int targetIds[3]{mark, bystander, friendly};
                for (size_t j = 0; j < 3; ++j) {
                    const float hp = hpOf(targetIds[j]);
                    if (hp < previousTargetHp[j]) ++damagedRecipients;
                    previousTargetHp[j] = hp;
                }
                if (drawsThisTick > 0) {
                    ++spreadDamageTicks;
                    if (drawsThisTick != damagedRecipients) perRecipientDrawsMatch = false;
                }
                if (!moved && !w.projectiles().empty()) {
                    flew = true;
                    // Yank the target well clear, so the shell cannot connect.
                    sim::Unit* m = w.unit(mark);
                    m->x = tak::sim::Fixed::fromInt(600); m->z = tak::sim::Fixed::fromInt(1400);
                    moved = true;
                }
            }
            check(flew, "the catapult actually fired");
            float by1 = hpOf(bystander);
            float friendly1 = hpOf(friendly);
            check(by1 < by0, "the shell still lands and splashes the aim point",
                  std::to_string(int(by0)) + " -> " + std::to_string(int(by1)));
            check(friendly1 < friendly0,
                  "retail ballistic splash damages a same-owner nearby unit",
                  std::to_string(int(friendly0)) + " -> " + std::to_string(int(friendly1)));
            check(spreadDamageTicks > 0 && perRecipientDrawsMatch,
                  "ballistic impact consumes exactly one deterministic CRT draw per damaged splash recipient",
                  std::to_string(damageSpreadDraws) + " draws across " +
                      std::to_string(spreadDamageTicks) + " damage ticks");
        }
    }
    // ...but a shot that CONNECTS must not also crater on the same tick (the
    // life<=0 expiry and the direct hit can coincide -- that is what `spent` guards).
    if (want("rules")) {
        std::printf("[a hit does not double-dip]\n");
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
            float hp0 = w.unit(sitting)->hp.toFloat();
            w.attack(gun, sitting, false);
            // Long enough for exactly one shell to be fired and to land, but well
            // inside the 6.5s reload so a second can't confuse the total.
            for (int i = 0; i < 30 * 6; ++i) w.tick(1.0f / 30.0f);
            float lost = hp0 - w.unit(sitting)->hp.toFloat();
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
    if (want("rules")) {
        std::printf("[units are solid]\n");
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
                const float bx0 = w.unit(blocker)->x.toFloat(), bz0 = w.unit(blocker)->z.toFloat();
                // RETAIL'S BOUND IS A CELL, NOT A PIXEL. Blocking is cell-granular
                // (the occupancy grid IS the collision model), so where a body stops
                // depends on how the two footprints land on the 16px grid: exact
                // pixel tangency is one alignment among many, and up to a cell of
                // visual overlap or gap is retail's own behaviour. What is NEVER
                // allowed is a body's centre inside another's footprint -- that is
                // the tunnel this check exists to catch (it reached 1px once).
                float closest = 1e9f;
                for (int i = 0; i < 30 * 15; ++i) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* m2 = w.unit(walker);
                    if (!m2 || !m2->alive()) break;
                    closest = std::min(closest, std::max(std::fabs(m2->x.toFloat() - bx0),
                                                         std::fabs(m2->z.toFloat() - bz0)));
                }
                check(closest >= 8.0f,
                      "a walker never drives into a parked body",
                      "closest approach " + std::to_string(int(closest)) +
                          "px (centre stays out of the stamped cells)");
                const sim::Unit* b = w.unit(blocker);
                const sim::Unit* m = w.unit(walker);
                // RETAIL ARRIVES. The search sees the parked body (it grades 0 on the
                // rating map), routes around it, and the walker reaches x=800. The
                // assertion that used to stand here -- walker held short of 760 --
                // pinned OUR pre-port behaviour, where bodies were invisible to the
                // search and the mover wedged against the blocker instead.
                const float ax = m->x.toFloat() - 800.0f, az = m->z.toFloat() - 600.0f;
                check(std::sqrt(ax * ax + az * az) < 48.0f,
                      "...and still arrives, because the route bends around the body",
                      "walker (" + std::to_string(int(m->x.toFloat())) + "," +
                          std::to_string(int(m->z.toFloat())) + "), goal (800,600)");
                check(std::fabs(b->x.toFloat() - bx0) < 1.0f &&
                          std::fabs(b->z.toFloat() - bz0) < 1.0f,
                      "...and nothing shoved the parked blocker",
                      "blocker moved to (" + std::to_string(int(b->x.toFloat())) + "," +
                          std::to_string(int(b->z.toFloat())) + ")");
            }
            // (a2) HEAD-ON, with the route running STRAIGHT THROUGH the body. Test
            //     (a) sends the walker PAST the blocker, and the search detours, so
            //     the head-on approach never actually happens there. Aim the order
            //     just beyond the body instead -- within the search's near-goal
            //     exemption -- and the route stays straight, which is the geometry
            //     that found a real tunnel: the first penetrating step landed inside
            //     the mover's 0.75px touch slack, was admitted, and the
            //     "already inside, not ours to arbitrate" gate then skipped the body
            //     for ever. Measured 20px deep at full speed before the gate learned
            //     to use the same slack as the admission.
            //
            //     Assert ONLY the invariant: never penetrate. Where the walker ENDS
            //     is mover policy (today it presses and holds; retail's mover would
            //     sidestep), but no policy is allowed to put one body inside another.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                sim::setupMatch(w, reg, cfg);
                int blocker = w.spawn(sw, 700, 600, 0, 0);
                int walker = w.spawn(sw, 640, 600, 0, 0);
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);
                const float bx0 = w.unit(blocker)->x.toFloat(), bz0 = w.unit(blocker)->z.toFloat();
                w.order(walker, 704, 600, false);   // just past the body: head-on
                float closest = 1e9f;
                float maxX = -1e9f;
                for (int i = 0; i < 30 * 10; ++i) {
                    w.tick(1.0f / 30.0f);
                    const sim::Unit* m2 = w.unit(walker);
                    if (!m2 || !m2->alive()) break;
                    closest = std::min(closest, std::max(std::fabs(m2->x.toFloat() - bx0),
                                                         std::fabs(m2->z.toFloat() - bz0)));
                    maxX = std::max(maxX, m2->x.toFloat());
                }
                // The bound is the STAMP BAND, not the body: blocking excludes the
                // walker's centre from the blocker's stamped cells, and a body's
                // stamp covers the aligned cells its centre touches -- so rounding a
                // corner can bring centres within about half a cell of the stamp
                // edge. 8px is the half-cell; anything under it means the centre got
                // INSIDE the stamp, which is the tunnel.
                check(closest >= 8.0f,
                      "head-on into a parked body never tunnels through it",
                      "closest approach " + std::to_string(int(closest)) +
                          "px (centre stays out of the stamped cells)");
                // The walker MAY end past the blocker's x -- the search fails (the
                // goal cell's footprint overlaps the body) and the best-effort route
                // walks it AROUND to the closest reachable spot, retail's "walks as
                // close as it can and comes to rest" (0x415170). What it may never do
                // is pass THROUGH: the closest-approach bound above is the invariant.
                // maxX is still computed so a tunnel regression prints usefully.
                (void)maxX;
                // "Rest" at a crowd-blocked goal is POSITIONAL, not order-complete:
                // retail keeps the order alive (the blocker might move) and the unit
                // stands at its closest approach between re-asks, pressing under the
                // refusal caps. Assert the stance -- stationary at the end -- not
                // empty orders.
                const sim::Unit* hw = w.unit(walker);
                float hx = hw->x.toFloat(), hz = hw->z.toFloat();
                for (int i = 0; i < 30 * 3; ++i) w.tick(1.0f / 30.0f);
                const float drift = std::max(std::fabs(hw->x.toFloat() - hx),
                                             std::fabs(hw->z.toFloat() - hz));
                check(drift < 4.0f,
                      "...and stands at its closest approach instead of churning",
                      "drifted " + std::to_string(int(drift)) + "px over 3s");
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
                // 40px pitch: footprints are 32px, so the ranks spawn CLEAR of each
                // other. The old 20px pitch spawned every unit 12px inside its
                // neighbours -- a state retail cannot produce (its placement test,
                // 0x507d10, refuses any overlapped spawn), so nothing about how the
                // sim digs itself out of it is retail behaviour worth pinning.
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 6; ++i)
                        army.push_back(w.spawn(sw, 600.0f + float(i) * 40.0f,
                                               600.0f + float(j) * 40.0f, 0, 0));
                for (int id : army) w.order(id, 900, 700, false);
                // Record each goal's failure-expanded radius before dispatch
                // removes it. Traffic-scoring tolerance is not mission arrival.
                std::vector<uint32_t> radii(army.size(), 0);
                for (int tick = 0; tick < 30 * 120; ++tick) {
                    for (size_t i = 0; i < army.size(); ++i)
                        for (const auto& goal : w.unit(army[i])->orders)
                            if (goal.groundMission) { radii[i] = goal.missionRadius; break; }
                    w.tick(1.0f / 30.0f);
                }
                int arrived = 0, givenUp = 0, close = 0;
                for (size_t i = 0; i < army.size(); ++i) {
                    const auto* u = w.unit(army[i]);
                    const float dx = u->x.toFloat() - 900, dz = u->z.toFloat() - 700;
                    const float accepted = float(radii[i] + 4) + 23.0f;
                    close += std::max(std::fabs(dx), std::fabs(dz)) <= 144;
                    if (dx * dx + dz * dz <= accepted * accepted) ++arrived;
                    else if (u->orders.empty()) ++givenUp;
                }
                check(arrived >= 21,
                      "an army of 24 reaches its failure-expanded mission circles",
                      std::to_string(arrived) + "/24 accepted; " + std::to_string(close) + "/24 within 144px");
                check(givenUp == 0,
                      "no army member abandons an order outside its mission circle",
                      std::to_string(givenUp) + " idle outside their circles");
            }
            // (c) REMOVED 2026-09-15. It asserted that two columns marching head-on
            //     pass through each other ("two moving units can never block one
            //     another"), which was true of the model we had before retail's
            //     occupancy rule was ported. It is not true of retail: that rule
            //     (RE'd at 0x4db767-0x4db7c9, see bodyPenetration) lets you close up
            //     behind someone going your way who is not slower than you, and
            //     BLOCKS head-on traffic. Two files sharing one z therefore wedge --
            //     "fluid local movement that occasionally wedges", docs/retail-engine.md.
            //
            //     It is not replaced by an assertion that they wedge, either: that
            //     would pin a deadlock down as required behaviour. Crowd flow is
            //     measured by tools/crowdbench (five scenarios, arrival rate the
            //     headline) -- all five are at 100% arrival, which is the property
            //     this test was reaching for. Measure there, not here.
        }
    }

    // Mobile units take their footprint from the movement class, not their own FBI:
    // 125 of 152 movers declare no footprintx at all, and not one is 1x1.
    if (want("rules")) {
        std::printf("[movementclass footprints]\n");
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
            if (t.maxVel <= sim::Fixed() || t.canFly) continue;
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
    if (want("rules")) {
        std::printf("[yardmap-aware placement]\n");
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
                int ox = sim::footprintOrigin(float(bx) * 16 + 8, keep->footX);
                int oz = sim::footprintOrigin(float(bz) * 16 + 8, keep->footZ);
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
    if (want("rules")) {
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
            // Both waypoints must be somewhere the unit can stand so this
            // exercises queue order rather than an unreachable-goal approach.
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
                float da = detmathLen(u->x.toFloat() - ax, u->z.toFloat() - az);
                float db = detmathLen(u->x.toFloat() - bx, u->z.toFloat() - bz);
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
                if (detmathLen(u->x.toFloat() - outX, u->z.toFloat() - outZ) < 48) { visitedOut = true; break; }
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
    if (want("fireatwill")) {
        auto rank = [&](const char* who, uint32_t seed, int& distinct, int& maxOnOne) {
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
            w.setGameSeed(seed);
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
            // A leashed swordsman's pick is an attack order; a hold-position
            // archer gets none (4d7de0: auto on Hold Position issues nothing)
            // and its pick is the fire-at-will sweep's slot target.
            std::map<int, int> tally;
            for (const auto& u : w.units()) {
                if (!u.alive() || u.player != 0) continue;
                int t = u.orders.empty() ? 0 : u.orders.front().targetId;
                for (int s = 0; s < 3 && !t; ++s) t = u.slotTarget[size_t(s)];
                if (t) tally[t]++;
            }
            distinct = int(tally.size());
            for (auto& [t, n] : tally) maxOnOne = std::max(maxOnOne, n);
        };
        const sim::UnitType* archer = reg.find("araarch");
        check(archer && archer->fireAtWillRandom, "araarch parses fireatwillrandom");
        const sim::UnitType* sword = reg.find("arasword");
        check(sword && !sword->fireAtWillRandom, "arasword does not carry it");
        int aDistinct = 0, aMax = 0, sDistinct = 0, sMax = 0;
        int firstDistinct=0,firstMaximum=0;
        // Spread is statistical; one valid creation-RNG sequence can tie.
        for (uint32_t seed=1;seed<=16;++seed) {
            int distinct,maximum;
            rank("araarch",seed,distinct,maximum); aDistinct+=distinct; aMax+=maximum;
            if (seed==1) { firstDistinct=distinct; firstMaximum=maximum; }
            rank("arasword",seed,distinct,maximum); sDistinct+=distinct; sMax+=maximum;
        }
        check(aDistinct > sDistinct,
              "archers cover more of the crowd across 16 creation seeds",
              "archers " + std::to_string(aDistinct) + " targets vs swordsmen " +
                  std::to_string(sDistinct));
        check(aMax <= sMax,
              "and pile onto one body no harder than the swordsmen do",
              "archers " + std::to_string(aMax) + " on one vs swordsmen " +
                  std::to_string(sMax));
        // The scatter is deterministic: same world, same draws, same answer.
        int d2 = 0, m2 = 0;
        rank("araarch", 1, d2, m2);
        check(d2 == firstDistinct && m2 == firstMaximum,
              "the scatter is deterministic (identical across two identical runs)");
    }

    // ---- standing orders: the stance buttons write TWO retail fields ---------
    if (want("orders")) {
        auto freshWorld = [&](sim::World& w) {
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            // Isolate stance/leash behavior from feature footprints and terrain.
            w.setTerrain(std::vector<uint8_t>(256 * 256, 100), 256, 256, 20);
        };
        // standingunitorder = 1 (Defensive): move 0 / fire 2.
        const sim::UnitType* archer = reg.find("araarch");
        check(archer && archer->defaultStandingOrder == 1 && archer->defaultMove == 0 && archer->defaultFire == 2,
              "araarch (standingunitorder=1) defaults to hold-position + fire-at-will");
        // standingunitorder = 2 (Offensive): move 1 / fire 2 -- a LEASHED chase.
        const sim::UnitType* knight = reg.find("araknigh");
        check(knight && knight->defaultStandingOrder == 2 && knight->defaultMove == 1 && knight->defaultFire == 2,
              "araknigh (standingunitorder=2) defaults to a leashed chase");
        // standingunitorder = 0 (Passive): move 0 / fire 0 -- holds fire entirely.
        const sim::UnitType* spy = reg.find("araspy");
        check(spy && spy->defaultStandingOrder == 0 && spy->defaultMove == 0 && spy->defaultFire == 0,
              "araspy (standingunitorder=0) defaults to holding fire");
        // No key at all -> the sentinel path: roam + fire at will. Worth noting
        // what this set turns out to be: almost every unit WITHOUT the key is a
        // structure (Keep, Watch Tower, wall, Mana Amplifier...) plus a couple of
        // flyers. So in retail essentially no mobile ground unit roams without a
        // leash -- they are all either hold-position or leashed at 500.
        const sim::UnitType* flyer = reg.find("arafly");
        // Retail's parser stores that composite as 2 (4c00a7), so GET 46 answers
        // 2 for such a type, never 3.
        check(flyer && flyer->defaultStandingOrder == 2 && flyer->defaultMove == 2 && flyer->defaultFire == 2,
              "arafly (no standingunitorder) defaults to unlimited roam, its composite order 2");
        if (flyer) {
            sim::World w; freshWorld(w);
            const int f = w.spawn(flyer, 1000, 1000, 0, 0);
            check(w.unit(f) && w.unit(f)->standingOrder == 2,
                  "a spawned arafly's standing unit order (GET 46) is 2");
        }
        const sim::UnitType* sword = reg.find("arasword");
        check(sword && sword->defaultMove == 1,
              "arasword (standingunitorder=2) is leashed, not roaming");

        // A hold-position unit shoots what comes to it but never walks over.
        if (archer && sword) {
            sim::World w; freshWorld(w);
            int a = w.spawn(archer, 1000, 2600, 0, 0);
            check(w.unit(a)->standingOrder == 1 && w.unit(a)->moveState == 0 && w.unit(a)->fireState == 2,
                  "the spawned archer carries its type's standing orders");
            float far = float(archer->maxRange()) + 260.0f;
            int prey = w.spawn(sword, 1000 + far, 2600, 0, 1);
            (void)prey;
            float startX = w.unit(a)->x.toFloat();
            for (int i = 0; i < 30 * 8; ++i) w.tick(1.0f / 30.0f);
            const sim::Unit* au = w.unit(a);
            check(au && std::fabs(au->x.toFloat() - startX) < 24.0f,
                  "a hold-position archer does NOT walk to an out-of-range enemy",
                  "moved " + std::to_string(au ? au->x.toFloat() - startX : 0.0f) + "px");

            // A leashed unit advances on something inside its leash...
            // The mission scan takes only an enemy the owner sees (4d8370 reads
            // the visible-enemy list), so the enemy stands inside the
            // swordsman's 135 px sight as well as its leash.
            sim::World w2; freshWorld(w2);
            int a2 = w2.spawn(sword, 1000, 2600, 0, 0);   // arasword: move 1, leash 500
            w2.spawn(sword, 1120, 2600, 0, 1);            // 120px: seen, inside the leash
            check(w2.unit(a2)->moveState == 1, "the swordsman is leashed");
            float sx2 = w2.unit(a2)->x.toFloat();
            for (int i = 0; i < 30 * 8; ++i) w2.tick(1.0f / 30.0f);
            check(w2.unit(a2) && w2.unit(a2)->x.toFloat() - sx2 > 24.0f,
                  "a leashed unit advances on an enemy inside its leash",
                  "moved " + std::to_string(w2.unit(a2) ? w2.unit(a2)->x.toFloat() - sx2 : 0.0f) + "px");

            // ...and stays put for one beyond it.
            sim::World w4; freshWorld(w4);
            int a4 = w4.spawn(sword, 1000, 2600, 0, 0);
            w4.spawn(sword, 1000 + sword->leash + 220.0f, 2600, 0, 1);
            float sx4 = w4.unit(a4)->x.toFloat();
            for (int i = 0; i < 30 * 8; ++i) w4.tick(1.0f / 30.0f);
            check(w4.unit(a4) && std::fabs(w4.unit(a4)->x.toFloat() - sx4) < 24.0f,
                  "but will not break its leash for one beyond it",
                  "moved " + std::to_string(w4.unit(a4) ? w4.unit(a4)->x.toFloat() - sx4 : 0.0f) + "px");

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
    if (want("orders")) {
        const sim::UnitType* fly = reg.find("arafly");
        check(fly && fly->canFly && fly->vtolStandby,
              "arafly parses defaultmissiontype = VTOL_standby");
        if (fly && fly->vtolStandby) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = "maps/Lake Lokken.tnt";   // needs open water
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            // Park it over open water. Retail's landing-site predicate 509400 has
            // no movement-class terrain test: an explored site is refused only for
            // features, yards, occupants, and the type's own depth/slope words,
            // and arafly (no movementclass, no maxslope) keeps the default
            // maxslope 255 (4dfb10/4dfc40 -> type +0x23c), so cliffs are landable.
            // What it does refuse a non-floater flyer is any cell whose quad low
            // lies below sea level (5094e3..509539), so water is the retail test.
            const auto& hm = w.mapHeights();
            const int mw = w.mapW(), mh = w.mapH(), sea = w.mapSea();
            auto wet = [&](int cx, int cz) {
                auto at = [&](int x, int z) {
                    return int(hm[size_t(std::min(z, mh - 1)) * mw + std::min(x, mw - 1)]);
                };
                return std::min(std::min(at(cx, cz), at(cx + 1, cz)),
                                std::min(at(cx, cz + 1), at(cx + 1, cz + 1))) < sea;
            };
            auto footprintWet = [&](float x, float z) {
                const int x0 = sim::footprintOrigin(x, fly->footX), z0 = sim::footprintOrigin(z, fly->footZ);
                for (int dz = 0; dz < fly->footZ; ++dz)
                    for (int dx = 0; dx < fly->footX; ++dx)
                        if (x0 + dx < 0 || z0 + dz < 0 || x0 + dx >= mw || z0 + dz >= mh || wet(x0 + dx, z0 + dz))
                            return true;
                return false;
            };
            float bx = 0, bz = 0; bool bad = false;
            for (int cz = 12; cz < mh - 12 && !bad; ++cz)
                for (int cx = 12; cx < mw - 12 && !bad; ++cx) {
                    bool open = true;   // offshore: a 4x4 block of water cells
                    for (int dz = -2; dz < 2 && open; ++dz)
                        for (int dx = -2; dx < 2 && open; ++dx) open = wet(cx + dx, cz + dz);
                    if (open) { bx = float(cx * 16); bz = float(cz * 16); bad = true; }
                }
            check(bad, "the map has open water no flyer could land on");
            if (bad) {
                int id = w.spawn(fly, bx, bz, 0, 0);
                // This fixture parks an airborne unit over an invalid landing
                // site; spawn itself starts at terrain height.
                w.unit(id)->flightGroundMode=2;
                w.unit(id)->flightY+=sim::Fixed::fromInt(fly->cruiseAlt);
                for (int i = 0; i < 30 * 20; ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                bool ok = u && u->flightGroundMode == 1 && !footprintWet(u->x.toFloat(), u->z.toFloat());
                check(ok, "an idle flyer relocates off water and lands on dry ground",
                      u ? "ended at " + std::to_string(int(u->x.toFloat())) + "," +
                              std::to_string(int(u->z.toFloat())) + " mode " + std::to_string(int(u->flightGroundMode))
                        : "gone");
                // And then it STAYS: the search must not re-trigger every tick and
                // leave the thing shuffling for the rest of the game.
                float sx = u ? u->x.toFloat() : 0, sz = u ? u->z.toFloat() : 0;
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u2 = w.unit(id);
                check(u2 && std::fabs(u2->x.toFloat() - sx) < 2.0f && std::fabs(u2->z.toFloat() - sz) < 2.0f,
                      "...and then settles, instead of shuffling forever");
            }
        }
    }

    // ---- unitstandorders: which types have a stance at all -------------------
    if (want("orders")) {
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
            if (!t.canSetStance && t.maxVel > sim::Fixed() && t.weapon.damage > 0)
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
    if (want("orders")) {
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
            check(u && u->x.toFloat() > 1350.0f,
                  "and THEN the builder goes on to the move queued behind it",
                  u ? "ended at x=" + std::to_string(int(u->x.toFloat())) : "gone");
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
    if (want("orders")) {
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
                if (!w.featureReclaimable(f)) continue;
                float dx = f.x.toFloat() - 900, dz = f.z.toFloat() - 2700;
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
    if (want("weapons")) {
        std::printf("[self-destruct]\n");
        const sim::UnitType* regen = nullptr;
        const sim::UnitType* norgen = nullptr;
        for (const auto& [tid, t] : reg.types()) {
            if (t.maxVel <= sim::Fixed()) continue;                  // movers only: easy to spawn
            if (!regen && t.healTime > 0) regen = &t;
            if (!norgen && t.healTime <= 0) norgen = &t;
        }
        if (!regen) std::printf("  (no regenerating unit type; skipped)\n");
        else {
            struct Case { const sim::UnitType* t; const char* label; };
            std::vector<Case> cases;cases.push_back({regen, "a REGENERATING unit"});
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
                check(!u || !u->alive(), (std::string(c.label) + " (" + c.t->id + ") is DEAD").c_str(),
                      u ? ("hp=" + std::to_string(u->hp.toFloat())) : "gone");
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
                    if (v && v->alive()) {
                        ++survived;
                        if (worst.empty())
                            worst = "phase " + std::to_string(phase) +
                                    (v ? " hp=" + std::to_string(v->hp.toFloat()) : " gone");
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
    if (want("orders")) {
        std::printf("\n[benchmark / stress unit placement]\n");
        auto misplaced = [&](sim::MatchConfig cfg, sim::World& w) {
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots.resize(8);
            for (int i = 0; i < 8; ++i) {
                cfg.slots[size_t(i)].used = true;
                cfg.slots[size_t(i)].team = i;
                cfg.slots[size_t(i)].faction = i % 5;
            }
            sim::setupMatch(w, reg, cfg);
            int total = 0, bad = 0;
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type || u.type->canFly) continue;
                ++total;
                // CENTRE cell, not the footprint: after 60s of fighting these have
                // MOVED, and retail's mover probes one cell -- a body's centre stays
                // on walkable ground while its footprint may overhang an unfittable
                // neighbour, which is the original's own look (units standing half
                // off a ledge). Footprint-fits belongs to PLACEMENT (0x507d10), and
                // the spawn itself is what the stress-fill check above pins.
                // TERRAIN only, and the CENTRE cell only. Footprint-fits belongs to
                // placement; after any fighting these have moved, and retail's
                // per-cell mover keeps a centre on standable ground while the body
                // may overhang. The obstacle overlay is excluded too: buildings
                // complete underneath units, and neither retail nor we shove the
                // unit off the new stamp.
                if (!w.navFor(u.type).terrainWalkable(int(u.x.toFloat()) / 16,
                                                      int(u.z.toFloat()) / 16)) ++bad;
            }
            return std::pair<int, int>{bad, total};
        };
        {
            sim::MatchConfig cfg;
            cfg.stressTest = true;
            cfg.stressImmediate = true;      // inspects the army right after setup
            cfg.unitCap = 2000;
            sim::World w;
            auto [bad, total] = misplaced(cfg, w);
            int flyers = 0, offMapFlyers = 0;
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type || !u.type->canFly) continue;
                ++flyers;
                const float hx = float(u.type->footX) * 8;
                const float hz = float(u.type->footZ) * 8;
                if (u.x.toFloat() < hx || u.z.toFloat() < hz ||
                    u.x.toFloat() > float(w.nav().width()) * 16 - hx ||
                    u.z.toFloat() > float(w.nav().height()) * 16 - hz)
                    ++offMapFlyers;
            }
            check(flyers > 0 && offMapFlyers == 0,
                  "stress flyers start with their footprints inside the map",
                  std::to_string(offMapFlyers) + "/" + std::to_string(flyers) + " outside");
            check(total > 1000, "the stress fill actually spawned an army",
                  std::to_string(total) + " units");
            check(total && bad * 100 / total < 3,
                  "stress-test units start on terrain they can occupy",
                  std::to_string(bad) + "/" + std::to_string(total) + " misplaced");
            // Terrain fit is only half of it: the snap also has to stop two bodies
            // being placed inside one another. `claimed` reserves a whole FOOTPRINT,
            // because reserving a centre cell let two 2x2 units take adjacent cells
            // 16px apart while needing 32 -- and the snap packs units exactly where
            // that bites, along a shoreline where the free cells are a thin line.
            //
            // ZERO is the bar, not a rate, and it is reachable because the fill no
            // longer over-subscribes: it asks for at most 95% of what the map can
            // actually hold. Before, on this map, it asked for 15200 bodies on room
            // for ~3300 and produced 19059 overlapping pairs -- the excess had nowhere
            // legal to go, so it went into terrain or into another body.
            long pairs = 0;
            {
                std::vector<const sim::Unit*> bodies;
                for (const auto& u : w.units())
                    if (u.alive() && u.type && !u.type->canFly) bodies.push_back(&u);
                for (size_t a = 0; a < bodies.size(); ++a)
                    for (size_t b = a + 1; b < bodies.size(); ++b) {
                        const float need =
                            // Each body's half-width is max(footX,footZ)*8 -- exactly what
                            // World::bodyPenetration uses -- so two bodies must be the SUM
                            // of those apart. An extra *0.5f here halved the requirement and
                            // passed two 2x2 units 16px apart when they need 32: the very
                            // case the comment above describes. A check that cannot fail on
                            // the bug it documents is worse than no check.
                            (float(std::max(bodies[a]->type->footX, bodies[a]->type->footZ)) +
                             float(std::max(bodies[b]->type->footX, bodies[b]->type->footZ)))
                            * 8.0f;
                        const float dx = bodies[a]->x.toFloat() - bodies[b]->x.toFloat();
                        const float dz = bodies[a]->z.toFloat() - bodies[b]->z.toFloat();
                        if (std::sqrt(dx * dx + dz * dz) < need) ++pairs;
                    }
            }
            check(pairs == 0, "and no two of them are placed inside each other",
                  std::to_string(pairs) + " overlapping pairs over " +
                      std::to_string(total) + " units");
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
            // The server's BenchClock, as at NORMAL speed: the ramp's stages fall due
            // evenly over the 60 seconds, then the run ends.
            const uint32_t stages = uint32_t(sim::benchmarkSpawns(cfg.benchmark));
            for (int i = 0; i < 1800; ++i) {
                w.benchmarkClock(uint32_t(uint64_t(stages) * uint32_t(i + 1) / 1800u), i == 1799);
                w.tick(1.0f / 30.0f);
            }
            int total = 0, bad = 0;
            for (const auto& u : w.units()) {
                if (!u.alive() || !u.type || u.type->canFly) continue;
                ++total;
                // Same semantics as the stress-fill check above: centre cell, base
                // terrain. These have fought for 60s; footprint overhang and
                // standing on a new building's stamp are both retail's own looks.
                if (!w.navFor(u.type).terrainWalkable(int(u.x.toFloat()) / 16,
                                                      int(u.z.toFloat()) / 16)) {
                    ++bad;
                    std::printf("    misplaced id=%d type=%s pos=(%.3f,%.3f) embarked=%d orders=%zu\n",
                                u.id,u.type->id.c_str(),u.x.toFloat(),u.z.toFloat(),int(u.embarked()),u.orders.size());
                }
            }
            // Much lower bar than the stress fill, for two reasons: this one has
            // fought for 60s first, and on a CRAMPED map the plan is capped at what
            // the map can hold. Inner Circle holds ~3300 bodies while intensity 5 asks
            // for 7680, so the cap binds hard here -- on Ulasem Arena, which is what
            // Benchmark actually runs, capacity is ~49000 and nothing is capped at all.
            check(total > 150, "the benchmark plan actually spawned an army",
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
    if (want("orders")) {
        std::printf("\n[per-category damage multiplier]\n");
        // The contract itself, on values rather than on shipped data. Spot-checking a
        // real unit is fragile here for a reason worth recording: the first version of
        // this test used aradrag's dragon=2, which sits on WEAPON2 (Fire Ball) while
        // `type->weapon` is WEAPON1 (Fire Breath) -- so it failed against CORRECT code
        // and looked like a second bug. The scaling rule is what matters; the siege
        // below covers the shipped data.
        sim::UnitType tgt{};
        tgt.categories = {"factory", "aramon"};
        tgt.damageCategory = "factory";
        sim::Weapon w{};
        w.damage = 360;
        check(w.damageVs(&tgt) == 360.0f, "no override -> base damage");
        w.dmgVs["factory"] = 180;
        check(w.damageVs(&tgt) == 180.0f,
              "a 0.5 override HALVES the base, it is not 0.5 damage",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs["factory"] = 720;
        check(w.damageVs(&tgt) == 720.0f, "a 2.0 override doubles it",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs["factory"] = 0.0f;
        check(w.damageVs(&tgt) == 0.0f, "a 0 override still means immune",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs.clear();
        w.dmgVs["naval"] = 90;      // a category this target does not carry
        check(w.damageVs(&tgt) == 360.0f, "an override for another category is ignored");
    }
    if (want("orders")) {
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
    if (want("orders")) {
        std::printf("\n[gods are summoned by the sim, not the client]\n");
        sim::World w;
        sim::MatchConfig cfg;
        cfg.vfs = &vfs;
        cfg.mapPath = kMap;
        cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
        cfg.slots[0].team = 0; cfg.slots[1].team = 1;
        cfg.slots[0].faction = 0; cfg.slots[1].faction = 1;
        cfg.slots[0].used = true; cfg.slots[1].used = true;   // else setupMatch skips them
        sim::setupMatch(w, reg, cfg);
        // The roll may or may not have enabled gods for this seed -- that is retail's
        // 10% GameChance. Force them on so the SUMMON path is what is under test.
        w.enableGods(0.0f);
        check(w.godsEnabled(), "gods are enabled");
        check(w.player(0).godType != nullptr,
              "setup resolved the player's god type (the sim cannot look it up)");

        const size_t before = w.units().size();
        // Nothing to prime: retail gates the arrival on the clock alone, and the
        // appear time above is 0, so the god is already due.
        check(w.godReady(0), "player 0 is ready for its god");

        tick(w, 0.2f);                       // nothing but World::tick runs here

        check(!w.godReady(0), "the readiness is consumed");
        check(w.player(0).godSummoned, "and the player is marked summoned");
        // BOTH players, and that is the retail rule rather than a loose assertion.
        // The arrival is gated on the clock alone (0x519420 decides IF and WHEN, and
        // nothing after that decides WHO), so once the time comes every player with a
        // god type gets one. This used to check `before + 1` and that only player 0
        // was summoned, which held only because our favour pool gated the other one.
        check(w.units().size() == before + 2,
              "ticking the world ALONE summoned a god for BOTH players",
              std::to_string(before) + " -> " + std::to_string(w.units().size()));
        auto hasGod = [&](int p) {
            for (const auto& u : w.units())
                if (u.alive() && u.player == p && u.type && u.type->id.size() > 3 &&
                    u.type->id.substr(u.type->id.size() - 3) == "god") return true;
            return false;
        };
        check(hasGod(0), "and the new unit is player 0's god");
        check(w.player(1).godSummoned && hasGod(1),
              "and player 1 got one too -- nothing gates WHO, only when");
    }

    // ---- canMove is not a structure test ------------------------------------
    //
    // CLAUDE.md's standing gotcha, with the data behind it. 13 shipped types declare
    // canmove=1 with no velocity and no bmcode, so canMove comes out TRUE for things
    // that are plainly buildings. The renderer's facing test used to read canMove, so
    // those 13 took the mover branch -- and since startBuild spawns every structure at
    // heading pi, they were drawn at -pi, turned 180 degrees. On a factory that puts
    // the stone build pad on the opposite side from retail, which is how it was
    // reported ("stone pad is to the north instead of to the south").
    //
    // This pins the DATA half: as long as a shipped type is a structure while
    // reporting canMove, isStructure() is the only safe discriminator and any code
    // that reaches for canMove to answer "is it a building" is wrong.
    if (want("orders")) {
        std::printf("\n[canMove is not a structure test]\n");
        int trap = 0;
        bool keepIsTrap = false;
        for (const auto& [id, t] : reg.types())
            if (t.isStructure() && (t.canMove || t.canFly)) {
                ++trap;
                if (id == "arakeep") keepIsTrap = true;
            }
        check(trap > 0, "shipped types report canMove while having no velocity",
              std::to_string(trap) + " of them");
        check(keepIsTrap, "the Barracks is one of them (the reported case)");
        const sim::UnitType* keep = reg.find("arakeep");
        check(keep && keep->isStructure(),
              "and isStructure() still answers correctly for it");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
