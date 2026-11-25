// retailgap_test: verifies the retail-parity sim fixes that have no other harness.
//
//   retailgap_test <retail-install-dir> [slice[,slice...]]
//
// The checks are grouped into slices (weapons, rules, fireatwill, orders) so ctest
// can run them as parallel tests; naming slices runs only those, none runs all.
//
// Checks, against the real shipped Total Annihilation data (GOG install):
//   1. [EXPLODEAS] parses, and a dying unit detonates it where it stands (splashing
//      nearby enemies) -- the crawling bomb's (Roach) whole purpose.
//   2. Weapon classes (Guided, Dropped) parse from weapons/*.tdf and behave.
//   3. A campaign mission loads, and its placed units' InitialMission= order queues
//      are applied; its .ota victory/defeat conditions resolve.
//
// A dev harness; not shipped in a game.

#include "hpi/hpi.h"
#include "sim/footprint.h"
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

using namespace ta;

namespace {
int failures = 0;

const char* const kSlices[] = {"weapons", "rules", "fireatwill", "orders"};
std::vector<std::string> selected;   // empty: every slice
bool want(const char* slice) {
    return selected.empty() || std::find(selected.begin(), selected.end(), slice) != selected.end();
}
std::string kMap;   // a shipped TA map (Ashap Plateau), set in main; we only need terrain

// Nudge a spawn point onto ground the unit actually fits on. Fixtures used to
// pick round numbers and rely on the sim's "unstick" pass to walk a unit off
// whatever rock it landed on; that pass is gone (retail has none), so a fixture
// that spawns on a cliff now simply has a unit that cannot move.
static void legalSpot(const ta::sim::World& w, const ta::sim::UnitType* t,
                      float& x, float& z) {
    const ta::sim::NavGrid& g = w.navFor(t);
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
// The top-left cell of a size x size block of cells where `t` fits everywhere,
// scanning from (fromX, fromZ): fixtures that need open ground find it rather than
// assume a coordinate on a particular map.
static bool openApron(const ta::sim::World& w, const ta::sim::UnitType* t, int size,
                      float& x, float& z, int fromX = 8, int fromZ = 8) {
    const ta::sim::NavGrid& g = w.navFor(t);
    const int foot = std::max(1, std::max(t->footX, t->footZ));
    for (int cz = fromZ; cz + size < g.height() - 8; cz += 2)
        for (int cx = fromX; cx + size < g.width() - 8; cx += 2) {
            bool clear = true;
            for (int dz = 0; dz <= size && clear; ++dz)
                for (int dx = 0; dx <= size && clear; ++dx)
                    if (!g.fits(cx + dx, cz + dz, foot)) clear = false;
            if (!clear) continue;
            x = float(cx) * 16 + 8;
            z = float(cz) * 16 + 8;
            return true;
        }
    return false;
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
    sim::setupRegistry(reg,vfs);   // units/, weapons/*.tdf and the build menus
    kMap = hpi::findMap(vfs, "Ashap Plateau");
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
        const sim::UnitType* rat = reg.find("corroach");      // Roach, the CORE crawling bomb
        const sim::UnitType* victim = reg.find("armpw");       // a plain Peewee
        if (!rat || !victim) { std::printf("  (missing unit defs; skipped)\n"); }
        else {
            check(rat->hasExplodeAs, "corroach parses an EXPLODEAS weapon");
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

            // A Roach (player 0) with two enemy Peewees (player 1) standing next to it.
            int ratId = w.spawn(rat, 500, 500, 0, 0);
            int aId = w.spawn(victim, 520, 500, 0, 1);
            int bId = w.spawn(victim, 500, 530, 0, 1);
            int farId = w.spawn(victim, 500, 1200, 0, 1);   // well outside the blast
            float aHp0 = w.unit(aId)->hp.toFloat(), farHp0 = w.unit(farId)->hp.toFloat();
            check(aHp0 > 0 && farHp0 > 0, "victims spawned alive");

            w.unit(ratId)->hp = sim::Fixed();    // the Roach dies -> its EXPLODEAS should fire
            tick(w, 0.2f);

            const sim::Unit* a = w.unit(aId);
            const sim::Unit* b = w.unit(bId);
            const sim::Unit* far = w.unit(farId);
            check(a && (!a->alive() || a->hp.toFloat() < aHp0), "adjacent enemy took the blast");
            check(b && (!b->alive() || b->hp.toFloat() < aHp0), "second adjacent enemy took the blast");
            check(far && far->alive() && far->hp.toFloat() >= farHp0, "distant enemy untouched");

            // Self-destruction selects the separate SELFDESTRUCTAS block: the
            // Roach's is CRAWL_BLAST, not its EXPLODEAS (CRAWL_BLASTSML).
            // (Kingdoms' stone/frozen deaths have no TA counterpart.)
            check(rat->hasSelfDestructAs && rat->selfDestructAs.section != rat->explodeAs.section,
                  "corroach carries its own SELFDESTRUCTAS block",
                  rat->selfDestructAs.section + " vs " + rat->explodeAs.section);
            {
                sim::World quiet;
                sim::setupMatch(quiet,reg,cfg);
                const int source=quiet.spawn(rat,500,500,0,0);
                quiet.spawn(victim,520,500,0,1);
                auto* dying=quiet.unit(source);
                dying->deathType=sim::Unit::kDeathSelfDestruct;
                dying->hp=sim::Fixed();
                // The death weapon fires at retirement (512ae0), once Dying is over.
                bool own=false,ordinary=false;
                for(int t=0;t<600;++t) {
                    quiet.tick(1.f/30);
                    for(const auto& hit:quiet.hits()) {
                        own=own || hit.weapon==&rat->selfDestructAs;
                        ordinary=ordinary || hit.weapon==&rat->explodeAs;
                    }
                    if(!quiet.unit(source)->deathPending)break;
                }
                check(!quiet.unit(source)->alive() && !quiet.unit(source)->deathPending,
                      "self-destruct death control reached the death transition");
                check(own && !ordinary, "self-destruct bursts its SELFDESTRUCTAS, not the ordinary death weapon");
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
    // TA's classes come from the weapon record's flags (docs/retail-data.md 3):
    // guidance/tracks = Guided, dropped = Dropped, ballistic shells, lineofsight
    // beams. (Kingdoms' Remote Effect / Wandering / mind control classes, the
    // per-shot veteran model and totalallowed have no TA counterpart.)
    if (want("weapons")) {
        std::printf("[weapon class parse]\n");
        struct C { const char* unit; int slot; sim::Weapon::Kind kind; const char* label; };
        const C cases[] = {
            {"armsam",   0, sim::Weapon::Kind::Guided,  "armsam W1 ARMTRUCK_MISSILE (tracks) = Guided"},
            {"armthund", 0, sim::Weapon::Kind::Dropped, "armthund W1 ARMBOMB (dropped) = Dropped"},
            {"armham",   0, sim::Weapon::Kind::Normal,  "armham W1 ARM_HAM (ballistic) = Normal"},
        };
        for (const C& c : cases) {
            const sim::UnitType* t = reg.find(c.unit);
            bool ok = t && int(t->weapons.size()) > c.slot && t->weapons[size_t(c.slot)].kind == c.kind;
            check(ok, c.label);
        }
        // turnrate=30000 TA angle units a second, held in radians/s.
        if (const sim::UnitType* t = reg.find("armsam"); t && !t->weapons.empty())
            check(std::abs(t->weapons[0].turnRate - 30000.0f * 6.2831853f / 65536.0f) < 0.01f,
                  "guided turnrate in radians/s", std::to_string(t->weapons[0].turnRate));
        if (const sim::UnitType* t = reg.find("armham"); t && !t->weapons.empty())
            check(t->weapons[0].ballistic && t->weapons[0].ballisticClass(), "the Hammer's plasma shell is ballistic");
    }

    // ---- 2b. guided homing --------------------------------------------------
    if (want("weapons")) {
        std::printf("[guided homing]\n");
        // armsam (Samson) has a GUIDED primary: ARMTRUCK_MISSILE (tracks=1), range
        // 600, velocity 540, turnrate 30000, reload 2.5s. One shot is in the air,
        // and only homing can bring it back onto a target that jumps aside mid-flight.
        const sim::UnitType* demon = reg.find("armsam");
        const sim::UnitType* prey = reg.find("armpw");
        if (!demon || !prey || demon->weapons.empty()) std::printf("  (missing defs; skipped)\n");
        else {
            check(demon->weapons[0].kind == sim::Weapon::Kind::Guided &&
                  demon->weapons[0].turnRate > 0, "the Samson's missile is guided");
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
            int mover = w.spawn(prey, 1000, 1500, 0, 1);   // 500px away, in range
            int peerShooter = peer.spawn(demon, 1000, 1000, 0, 0);
            int peerMover = peer.spawn(prey, 1000, 1500, 0, 1);
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
            check(!w.projectiles().empty(), "the Samson launched a guided shot");
            check(paired && w.stateHash()==peer.stateHash(),
                  "paired World instances retain the same Guided state hash at launch");
            if (!w.projectiles().empty()) {
                const auto& shot=w.projectiles().front();
                check(shot.guided3d && shot.targetId==mover && shot.fromId==shooter && shot.slot==0,
                      "Guided launch stores its owner, slot, target, and native XYZ trajectory");
                // TotalA.exe runs the units (0x4954ed) before the projectiles
                // (0x495513): the missile (0x49c9c0, +0x42 = the fire tick) has
                // taken its first step (0x49bb5b) when its fire tick ends.
                std::array<int32_t,3> step=shot.muzzle;
                for(size_t a=0;a<3;++a)step[a]=int32_t(uint32_t(step[a])+uint32_t(shot.velocity[a]));
                check(shot.taSelfProp && shot.position==step && shot.x.v==shot.position[0] &&
                      shot.z.v==shot.position[2] && shot.start==w.tickCount() && shot.age==1 && shot.end>shot.start,
                      "the missile leaves the script muzzle and takes its first step in its fire tick");
                // Jump the target 300px sideways: a straight shot now misses by far
                // more than the hit radius.
                if (auto* m = w.unit(mover)) { m->x = ta::sim::Fixed::fromInt(1300); m->orders.clear(); }
                if (auto* m = peer.unit(peerMover)) { m->x = ta::sim::Fixed::fromInt(1300); m->orders.clear(); }
                float hp0 = w.unit(mover)->hp.toFloat();
                for (int i = 0; i < 60; ++i) {
                    w.tick(1.0f / 30.0f);peer.tick(1.0f / 30.0f);   // 2s < the 2.5s reload
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
        const sim::UnitType* flyerBase=reg.find("armatlas");
        const sim::UnitType* demon=reg.find("armsam");
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
                  "the Samson's missile is a projectile-backed Guided weapon that can hit flyers");
            sim::World w,peer;
            sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=kMap;
            cfg.slots={sim::MatchSlot{},sim::MatchSlot{}};
            cfg.slots[0].team=0;cfg.slots[1].team=1;
            sim::setupMatch(w,reg,cfg);
            sim::setupMatch(peer,reg,cfg);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            peer.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            const int shooter=w.spawn(demon,1000,1000,0,0);
            const int target=w.spawn(&harmlessFlyer,1000,1500,0,1);
            const int peerShooter=peer.spawn(demon,1000,1000,0,0);
            const int peerTarget=peer.spawn(&harmlessFlyer,1000,1500,0,1);
            if(auto* u=w.unit(shooter))u->stance=0;
            if(auto* u=peer.unit(peerShooter))u->stance=0;
            for(auto [world,id]:{std::pair{&w,target},std::pair{&peer,peerTarget}}) {
                if(auto* u=world->unit(id)) {
                    u->flightGroundMode=2;
                    u->flightY=sim::Fixed::fromInt(320);
                }
                // An explicit flight mission prevents idle VTOL standby from
                // beginning a landing during the attacker's aim/build-up ticks.
                world->order(id,1080,1500,false);
            }
            w.attack(shooter,target,false);
            peer.attack(peerShooter,peerTarget,false);
            const auto launchAim=w.queryWeaponAim(shooter,target,0);
            const auto peerLaunchAim=peer.queryWeaponAim(peerShooter,peerTarget,0);
            check(launchAim.has_value() && peerLaunchAim.has_value() &&
                  launchAim->heading==peerLaunchAim->heading && launchAim->pitch==peerLaunchAim->pitch &&
                  launchAim->target==peerLaunchAim->target,
                  "paired Samsons compute the same authored aim solution for the flying target");
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
            check(sawFireEvent,"the Samson releases its Guided weapon against the airborne target");
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
            // TotalA.exe 0x49b3e0: a target that is no longer alive is no
            // longer chased; the shot steers for its launch point (+0x28)
            // and flies on, powered, until its expiry.
            if(!w.projectiles().empty()) {
                const auto point=w.projectiles().front().taTargetPoint;
                const auto position=w.projectiles().front().position;
                for(int i=0;i<3;++i) {
                    w.tick(1.0f/30.0f);peer.tick(1.0f/30.0f);
                    paired=paired && w.stateHash()==peer.stateHash();
                }
                if(!w.projectiles().empty()) {
                    const auto& shot=w.projectiles().front();
                    lostTargetContinues=shot.guided3d && shot.taSelfProp && shot.targetId==target &&
                        shot.taTargetPoint==point && shot.position!=position;
                }
            }
            check(lostTargetContinues,"losing the target leaves a Guided shot flying for its launch point");
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
        const sim::UnitType* demon=reg.find("armsam");
        const sim::UnitType* prey=reg.find("armpw");
        const sim::UnitType* roc=reg.find("armatlas");
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
                *victim=world->spawn(prey,1000,1500,0,1);
                *carrier=world->spawn(roc,1500,1500,0,1);
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
                  "the Samson launched a guided shot at the soon-carried target");
            if(!w.projectiles().empty()) {
                const auto launch=w.projectiles().front().velocity;
                const bool boarded=w.embarkInto(target,host) && peer.embarkInto(peerTarget,peerHost);
                check(boarded,"the target boards the Atlas while the shot is in the air");
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

    // ---- 2c. (Kingdoms' Remote Effect / mind control spells) --------------
    // Removed with the port to TA: TA has no remote-effect, hail, stone or mind
    // control weapons (weapons/*.tdf carry none of those classes).

    // ---- 2d. dropped bombs ---------------------------------------------------
    if (want("rules")) {
        std::printf("[dropped bombs]\n");
        // armthund's ARMBOMB is dropped=1: released from the flyer and falls onto
        // the ground beneath it.
        const sim::UnitType* bomber = reg.find("armthund");
        const sim::UnitType* prey = reg.find("armpw");
        if (!bomber || !prey || bomber->weapons.empty()) std::printf("  (missing defs; skipped)\n");
        else {
            int slot = -1;
            for (size_t i = 0; i < bomber->weapons.size(); ++i)
                if (bomber->weapons[i].kind == sim::Weapon::Kind::Dropped) { slot = int(i); break; }
            check(slot >= 0, "armthund carries a Dropped weapon");
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int b = w.spawn(bomber, 1000, 1000, 0, 0);
            int v = w.spawn(prey, 1000, 1500, 0, 1);   // 500px away: well beyond a drop
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
            // How close a TA bomber flies before release is not pinned by the
            // spec (docs/retail-*.md); what matters here is that the dropped bomb
            // reaches the target.
            check(hurt, "the bomber's dropped bomb landed on the target",
                  "closest approach " + std::to_string(int(closest)) + "px");
        }
    }

    // ---- 2e. multi-weapon units ---------------------------------------------
    // (Kingdoms' weaponswitching casters have no TA counterpart: no TA FBI
    // carries weaponswitching, so every TA unit works all of its weapons.)
    if (want("rules")) {
        std::printf("[multi-weapon units]\n");
        const sim::UnitType* prey = reg.find("armpw");
        // A multi-weapon unit WITHOUT weaponswitching fires every weapon
        // independently: the Warrior's Light Cannon and E.M.G. both work.
        const sim::UnitType* tower = reg.find("armwar");
        if (tower && prey && tower->weapons.size() > 1) {

            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int t = w.spawn(tower, 1000, 1000, 0, 0);
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
            // Not a check: armwar's AimPrimary and AimSecondary send and mask the
            // same signal (docs/retail-weapon-scripts.md 8.3), so the Light
            // Cannon's aim dies whenever the E.M.G. aims in the same tick and it
            // may never fire while both hold targets. What is checked is that the
            // unit fires at all and no weapon switching holds a slot back.
            std::printf("    armwar fired %d of its weapons\n", n);
            check(n >= 1, "armwar fires with no weapon switching holding a slot back",
                  std::to_string(n) + " weapons fired");
        }
    }

    // ---- 3. campaign mission + InitialMission --------------------------------
    // TA's campaign missions are pure data (maps/acNN.ota, no god script): the
    // placed units' InitialMission= order queues (docs/retail-campaign.md 5.3)
    // are the NPC choreography. ac06 queues factory waves ("b CORAK ...").
    if (want("rules")) {
        std::printf("[campaign mission + InitialMission]\n");
        const std::string stem = "ac06";
        check(sim::isTaMission(vfs, stem), "ac06 is a TA campaign mission (maps/ac06.ota)");
        sim::World w;
        int human = 0;
        bool ok = sim::setupMission(w, reg, vfs, stem, human);
        check(ok, "the mission loads");
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

    // ---- 4. campaign conditions ----------------------------------------------
    // The .ota [GlobalHeader] rules (docs/retail-campaign.md 5.4): polled once a
    // second, and an outcome lands after a 5 s countdown.
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
        // ac03 is an escort: MoveUnitToRadius=ARMSPID,2058,2129,256 is the victory
        // and AllUnitsKilledOfType=ARMSPID the defeat.
        const sim::UnitType* spider = reg.find("armspid");
        {
            sim::World w;
            const bool loaded = runMission("ac03", w);
            check(loaded && spider, "ac03 loads");
            if (loaded && spider) {
                tickM(w, 2.0f);
                check(w.missionOutcome() == 0, "escort mission does not resolve at the start");
                int killed = 0;
                for (auto& u : w.units())
                    if (u.type == spider && u.player == 0)
                        if (auto* p = w.unit(u.id)) { p->hp = sim::Fixed(); ++killed; }
                tickM(w, 8.0f);
                check(killed > 0 && w.missionOutcome() < 0, "losing every escorted Spider is a DEFEAT",
                      std::to_string(killed) + " killed, outcome=" + std::to_string(w.missionOutcome()));
            }
        }
        {
            sim::World w;
            if (runMission("ac03", w) && spider) {
                tickM(w, 2.0f);
                int moved = 0;
                for (auto& u : w.units())
                    if (u.alive() && u.type == spider && u.player == 0 && !moved)
                        if (auto* p = w.unit(u.id)) {
                            p->x = ta::sim::Fixed::fromInt(2058); p->z = ta::sim::Fixed::fromInt(2129);
                            p->orders.clear(); ++moved;
                        }
                tickM(w, 8.0f);
                check(moved > 0 && w.missionOutcome() > 0,
                      "getting a Spider inside the authored radius is a VICTORY",
                      "outcome=" + std::to_string(w.missionOutcome()));
            }
        }
        // ac01: AllUnitsKilledOfType=ARMGATE -- the human must keep its Galactic
        // Gate alive.
        {
            sim::World w;
            const sim::UnitType* gate = reg.find("armgate");
            if (runMission("ac01", w) && gate) {
                tickM(w, 2.0f);   // let the condition arm (it must not fire at t=0)
                check(w.missionOutcome() == 0, "ac01 does not resolve at the start");
                int killed = 0;
                for (auto& u : w.units())
                    if (u.type == gate) if (auto* p = w.unit(u.id)) { p->hp = sim::Fixed(); ++killed; }
                tickM(w, 8.0f);
                check(killed > 0 && w.missionOutcome() < 0, "AllUnitsKilledOfType protects YOUR units",
                      "outcome=" + std::to_string(w.missionOutcome()));
            }
        }
        // ac02 wins on KillUnitType=CORLAB,1: the human's own losses are no
        // victory; killing the enemy's Kbot Lab is.
        for (bool ownSide : {true, false}) {
            sim::World w;
            int human = 0;
            if (!sim::setupMission(w, reg, vfs, "ac02", human)) { check(false, "ac02 loads"); continue; }
            const sim::UnitType* lab = reg.find("corlab");
            tickM(w, 2.0f);
            check(w.missionOutcome() == 0, "a kill-count victory does not fire at the start");
            int killed = 0;
            for (auto& u : w.units())
                if (u.alive() && (ownSide ? u.player == human && !u.type->commander
                                          : u.player != human && u.type == lab))
                    if (auto* p = w.unit(u.id)) { p->hp = sim::Fixed(); ++killed; }
            tickM(w, 8.0f);
            if (ownSide)
                check(killed > 0 && w.missionOutcome() != 1, "ac02: losing your own army is no victory",
                      "outcome=" + std::to_string(w.missionOutcome()));
            else
                check(killed > 0 && w.missionOutcome() == 1, "ac02: destroying the CORE Kbot Lab is the victory",
                      std::to_string(killed) + " labs, outcome=" + std::to_string(w.missionOutcome()));
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
                                          cfg.slots[size_t(i)].faction = i % 2;   // ARM / CORE
                                          cfg.slots[size_t(i)].team = uint8_t(i); }
            return sim::setupMatch(w, reg, cfg);
        };
        auto fixedA = startsFor(false, 111), fixedB = startsFor(false, 222);
        check(fixedA == fixedB, "fixed starts ignore the seed");
        auto randA = startsFor(true, 111), randA2 = startsFor(true, 111);
        check(randA == randA2, "the same seed reproduces the same arrangement");
        auto randB = startsFor(true, 999);
        // The map's whole start table: Ashap Plateau authors ten StartPos
        // specials, so a 4-seat shuffle draws its spots from all ten.
        std::vector<std::pair<float, float>> table;
        {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots.resize(4);
            for (int i = 0; i < 4; ++i) { cfg.slots[size_t(i)].used = true; cfg.slots[size_t(i)].faction = i % 2;
                                          cfg.slots[size_t(i)].team = uint8_t(i); }
            sim::setupMatch(w, reg, cfg, &table);
        }
        bool moved = randA != fixedA || randB != fixedA;
        check(moved, "a shuffled match doesn't just hand back the fixed order");
        bool sameSet = !randA.empty();
        for (const auto* arrangement : {&randA, &randB})
            for (const auto& spot : *arrangement)
                sameSet = sameSet && std::find(table.begin(), table.end(), spot) != table.end();
        check(sameSet, "the shuffle draws from the map's starts, it doesn't invent them",
              std::to_string(table.size()) + " starts in the table");
    }

    // --- Tier-4 "cosmetic lows" ------------------------------------------
    // Several of these are display-only, but they all start with an FBI field that
    // must actually reach UnitType/Weapon -- which is the part that silently rots.
    // (Kingdoms' ghost and transportdistance keys have no TA counterpart.)
    if (want("rules")) {
        std::printf("[cosmetic lows: FBI fields reach the registry]\n");
        auto ut = [&](const char* id) { return reg.find(id); };
        if (const auto* tide = ut("armtide"))
            check(tide->noShadow, "armtide carries noshadow");
        if (const auto* boat = ut("armpt"))
            check(boat->floater, "armpt is a floater");
        if (const auto* amph = ut("armamph")) {
            check(amph->canHover, "armamph wades (canhover)");
            check(amph->waterline == 9, "and sinks to its waterline",
                  "waterline=" + std::to_string(amph->waterline));
        }
        // (turninplacerate is a Kingdoms key with no TA counterpart.)
    }
    // The turn/brake coupling replaced a flat "big turn -> 30% speed" cliff with
    // retail's arc-distance rule. The risk is a unit that brakes forever and never
    // arrives, so test arrival on a straight run AND on a right-angle dogleg.
    if (want("rules")) {
        std::printf("[turn/brake coupling]\n");
        const sim::UnitType* sw = reg.find("armpw");
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
            float ox = 0, oz = 0;
            check(openApron(w, sw, 24, ox, oz), "the map has an open corridor for the run");
            auto runTo = [&](float sz, float head, float secs) {
                int id = w.spawn(sw, ox, sz, head, 0);
                w.order(id, ox + 300, sz, false);
                for (int i = 0; i < int(secs * 30); ++i) w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                float dx = u->x.toFloat() - (ox + 300.0f), dz = u->z.toFloat() - sz;
                return std::sqrt(dx * dx + dz * dz);
            };
            float aligned = runTo(oz + 64, 1.5707963f, 20.0f);
            check(aligned < 48.0f, "a 300px run with an aligned start arrives",
                  std::to_string(int(aligned)) + "px short");
            float turning = runTo(oz + 160, 0.0f, 20.0f);
            check(turning < 48.0f, "and so does a 90-degree departure",
                  std::to_string(int(turning)) + "px short");
        }
    }

    // --- ballistic shells vs line-of-sight shots -------------------------
    // TA has no lobpreferred / gravityadjustment / dontleadtargets keys (those
    // checks were Kingdoms'); what carries over is which shots need the 2D clear
    // line: a ballistic shell's 3D arc decides (retail), a lineofsight shot does.
    if (want("rules")) {
        std::printf("[ballistic: fields + clear line]\n");
        auto wpn = [&](const char* id, int slot) -> const sim::Weapon* {
            const sim::UnitType* t = reg.find(id);
            if (!t || int(t->weapons.size()) <= slot) return nullptr;
            return &t->weapons[size_t(slot)];
        };
        if (const sim::Weapon* w = wpn("armham", 0)) {
            check(w->ballisticClass() && !w->needsClearLine(), "the Hammer's plasma shell needs no 2D line");
            check(w->aoe > 0, "its shell has a blast radius");
        }
        if (const sim::Weapon* w = wpn("cormort", 0))
            check(w->ballisticClass() && !w->needsClearLine(), "nor does the Morty's");
        if (const sim::Weapon* w = wpn("armpw", 0))
            check(w->straight && w->needsClearLine(), "a lineofsight E.M.G. shot does");
        if (const sim::Weapon* w = wpn("corak", 0))
            check(w->beam && w->needsClearLine(), "and so does a laser beam");
    }
    // A missed splash shell must crater. Fire a catapult, wait for the shell to be
    // in the air, then move the target out from under it -- the shell must still
    // land where it was aimed and splash whatever is standing there.
    // Every wall-like structure must actually block: both sides' Dragon's Teeth
    // and Fortification Walls.
    if (want("rules")) {
        std::printf("[walls block, both sides]\n");
        for (const char* id : {"armdrag", "cordrag", "armfort", "corfort"}) {
            const sim::UnitType* t = reg.find(id);
            if (!t) continue;
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            sim::setupMatch(w, reg, cfg);
            // Find open ground rather than assuming a cell: a hardcoded cell on
            // blocked terrain tests nothing.
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
        const sim::UnitType* pult = reg.find("corgol");   // Goliath: Heavy Cannon, ballistic, aoe 64
        const sim::UnitType* vic = reg.find("armpw");
        if (pult && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            float gx = 0, gz = 0;
            check(openApron(w, pult, 30, gx, gz), "the map has open ground for the range");
            gz += 160;
            int gun = w.spawn(pult, gx, gz, 0, 0);
            int mark = w.spawn(vic, gx + 300, gz, 0, 1);
            // A body apart from the mark (a footprint is 32px): bodies spawned
            // inside each other share the body plane's cells, and the splash
            // reads one occupant per cell.
            int bystander = w.spawn(vic, gx + 332, gz, 0, 1);   // beside the aim point
            int friendly = w.spawn(vic, gx + 300, gz - 32, 0, 0); // retail blast also hits allies
            // The splashed Peewees see each other: on Fire At Will they fight,
            // and their own shots draw at the same site. Holding fire, every
            // draw here is the shell's.
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
                    m->x = ta::sim::Fixed::fromFloat(gx); m->z = ta::sim::Fixed::fromFloat(gz + 400); m->orders.clear();
                    moved = true;
                }
            }
            check(flew, "the Goliath actually fired");
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
        const sim::UnitType* pult = reg.find("corgol");
        const sim::UnitType* vic = reg.find("armcom");   // tough enough to survive
        if (pult && vic) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs;
            cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            float gx = 0, gz = 0;
            openApron(w, pult, 30, gx, gz);
            gz += 160;
            int gun = w.spawn(pult, gx, gz, 0, 0);
            int sitting = w.spawn(vic, gx + 300, gz, 0, 1);   // stationary: no lead, direct hit
            w.setStance(sitting, 2);                         // hold fire: no reply to soak shells
            float hp0 = w.unit(sitting)->hp.toFloat();
            w.attack(gun, sitting, false);
            // Count the shells that left the barrel, and allow for the last one
            // still in flight: the loss must stay within one shell's damage per shell.
            int shells = 0;
            for (int i = 0; i < 30 * 6; ++i) {
                w.tick(1.0f / 30.0f);
                if (const sim::Unit* g = w.unit(gun); g && g->justFired) ++shells;
            }
            w.stop(gun);                                         // no more shells...
            for (int i = 0; i < 60; ++i) w.tick(1.0f / 30.0f);   // ...and the last one lands
            float lost = hp0 - w.unit(sitting)->hp.toFloat();
            const sim::Weapon& cw = pult->weapons[0];
            float once = cw.damageVs(vic);
            check(shells > 0 && lost > 0 && lost <= once * 1.50f * float(shells),
                  "each shell deals about one shell's damage, not two",
                  std::to_string(int(lost)) + " lost to " + std::to_string(shells) + " shells of " +
                      std::to_string(int(once)));
        }
    }

    // --- units are solid --------------------------------------------------
    // The dangerous change in this area is not "does blocking work" but "does an
    // army still move". Test both, and test the corridor case that deadlocks first.
    if (want("rules")) {
        std::printf("[units are solid]\n");
        const sim::UnitType* sw = reg.find("armpw");
        if (sw) {
            // (a) A parked body blocks: walk one unit into a stationary one and
            //     assert it cannot pass through.
            {
                sim::World w;
                sim::MatchConfig cfg;
                cfg.vfs = &vfs; cfg.mapPath = kMap;
                cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
                sim::setupMatch(w, reg, cfg);
                float ox = 0, oz = 0;
                check(openApron(w, sw, 26, ox, oz), "open ground for the body fixtures");
                oz += 128;   // the row through the middle of the apron
                int blocker = w.spawn(sw, ox + 60, oz, 0, 0);
                int walker = w.spawn(sw, ox, oz, 0, 0);
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);   // settle
                w.order(walker, ox + 160, oz, false);                      // straight through
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
                const float ax = m->x.toFloat() - (ox + 160), az = m->z.toFloat() - oz;
                check(std::sqrt(ax * ax + az * az) < 48.0f,
                      "...and still arrives, because the route bends around the body",
                      "walker (" + std::to_string(int(m->x.toFloat())) + "," +
                          std::to_string(int(m->z.toFloat())) + "), goal (" +
                          std::to_string(int(ox + 160)) + "," + std::to_string(int(oz)) + ")");
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
                float ox = 0, oz = 0;
                openApron(w, sw, 26, ox, oz);
                oz += 128;
                int blocker = w.spawn(sw, ox + 60, oz, 0, 0);
                int walker = w.spawn(sw, ox, oz, 0, 0);
                for (int i = 0; i < 30 * 10; ++i) w.tick(1.0f / 30.0f);
                const float bx0 = w.unit(blocker)->x.toFloat(), bz0 = w.unit(blocker)->z.toFloat();
                w.order(walker, ox + 64, oz, false);   // just past the body: head-on
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
                float ox = 0, oz = 0;
                check(openApron(w, sw, 26, ox, oz), "open ground for the army");
                std::vector<int> army;
                // 40px pitch: footprints are 32px, so the ranks spawn CLEAR of each
                // other. The old 20px pitch spawned every unit 12px inside its
                // neighbours -- a state retail cannot produce (its placement test,
                // 0x507d10, refuses any overlapped spawn), so nothing about how the
                // sim digs itself out of it is retail behaviour worth pinning.
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 6; ++i)
                        army.push_back(w.spawn(sw, ox + float(i) * 40.0f,
                                               oz + float(j) * 40.0f, 0, 0));
                const float gx = ox + 300, gz = oz + 100;
                for (int id : army) w.order(id, gx, gz, false);
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
                    const float dx = u->x.toFloat() - gx, dz = u->z.toFloat() - gz;
                    const float accepted = float(radii[i] + 4) + 23.0f;
                    close += std::max(std::fabs(dx), std::fabs(dz)) <= 144;
                    if (dx * dx + dz * dz <= accepted * accepted) ++arrived;
                    else if (u->orders.empty()) ++givenUp;
                }
                // TA's move is a circle of the order radius + 4 (0x403238), and a
                // goal the navigator cannot reach -- here, a point the crowd has
                // already filled -- ends the order with 0x40 where the unit stands
                // (docs/retail-pathfinding.md 2, 3.1). So the Kingdoms check that
                // every member reaches a failure-expanded circle is gone; what
                // stays is that the army MOVES and gathers at the goal.
                (void)arrived; (void)givenUp;
                check(close >= 21,
                      "an army of 24 crosses open ground and gathers at its goal",
                      std::to_string(close) + "/24 within 144px; " + std::to_string(arrived) + "/24 inside the order circle");
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

    // Mobile units take their footprint from the movement class: retail ignores
    // the FBI's own footprint when the class is found (docs/retail-data.md 2.2).
    if (want("rules")) {
        std::printf("[movementclass footprints]\n");
        struct Want { const char* id; int f; const char* cls; };
        const Want want[] = {
            {"armpw", 2, "TANKSH2"}, {"armcom", 2, "TANKDS2"},
            {"armsh", 3, "TANKHOVER3"}, {"armspid", 3, "SPID3"},
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

    // (Kingdoms' "a '.' yardmap cell is not footprint" placement check: no TA
    // yardmap carries '.', docs/retail-data.md 2.4.)

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
        const sim::UnitType* sword = reg.find("armpw");
        check(sword && sword->canMove && !sword->isBuilder, "armpw is a plain mobile non-builder");
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
            float homeX = 0, homeZ = 0;              // open ground with a real route
            openApron(w2, sword, 28, homeX, homeZ);
            homeZ += 128;
            const float outX = homeX + 400, outZ = homeZ;   // leg 1: east, AWAY from...
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

    // ---- auto-acquisition is deterministic ----------------------------------
    // Kingdoms' fireatwillrandom (missile troops scattering their fire) has no TA
    // counterpart: no TA FBI carries the key. What stays is the property the
    // spread check leaned on: auto-acquired picks come from the synced draws, so
    // the same world picks the same targets on every peer.
    if (want("fireatwill")) {
        std::printf("[auto-acquisition]\n");
        int flagged = 0;
        for (const auto& [id, t] : reg.types()) if (t.fireAtWillRandom) ++flagged;
        check(flagged == 0, "no TA unit carries Kingdoms' fireatwillrandom", std::to_string(flagged));
        auto rank = [&](const char* who, uint32_t seed, int& distinct, int& maxOnOne, int& picks) {
            const sim::UnitType* sh = reg.find(who);
            const sim::UnitType* prey = reg.find("armpw");
            distinct = maxOnOne = picks = 0;
            if (!sh || !prey) return;
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            w.setGameSeed(seed);
            float ox = 0, oz = 0;
            openApron(w, sh, 24, ox, oz);
            // Two ranks, spaced so no body starts inside another (32px footprints).
            for (int i = 0; i < 12; ++i)
                w.spawn(sh, ox + float(i % 6) * 36, oz + float(i / 6) * 36, 0, 0);
            for (int i = 0; i < 8; ++i)
                w.spawn(prey, ox + 50 + float(i % 4) * 36, oz + 160 + float(i / 4) * 36, 0, 1);
            for (int i = 0; i < 10; ++i) w.tick(1.0f / 30.0f);
            std::map<int, int> tally;
            for (const auto& u : w.units()) {
                if (!u.alive() || u.player != 0) continue;
                int t = u.orders.empty() ? 0 : u.orders.front().targetId;
                for (int s = 0; s < 3 && !t; ++s) t = u.slotTarget[size_t(s)];
                if (t) { tally[t]++; ++picks; }
            }
            distinct = int(tally.size());
            for (auto& [t, n] : tally) maxOnOne = std::max(maxOnOne, n);
        };
        int d1 = 0, m1 = 0, p1 = 0, d2 = 0, m2 = 0, p2 = 0;
        rank("armham", 1, d1, m1, p1);
        rank("armham", 1, d2, m2, p2);
        check(p1 > 0, "a rank of Hammers auto-acquires the Peewees in range",
              std::to_string(p1) + " picks on " + std::to_string(d1) + " targets");
        check(d1 == d2 && m1 == m2 && p1 == p2,
              "the picks are deterministic (identical across two identical runs)");
    }

    // ---- standing orders: standingmoveorder / standingfireorder -----------
    // TA keeps the two retail fields as separate FBI keys (docs/retail-data.md
    // 2.2 flagsA bits 0-3, both default 2): move 0 hold / 1 maneuver (leashed by
    // maneuverleashlength) / 2 roam; fire 0 hold / 1 return / 2 at will.
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
        // StandingMoveOrder=0, StandingFireOrder=2: the Commander holds position.
        const sim::UnitType* holder = reg.find("armcom");
        check(holder && holder->defaultMove == 0 && holder->defaultFire == 2,
              "armcom (StandingMoveOrder=0) defaults to hold-position + fire-at-will");
        // StandingMoveOrder=1, StandingFireOrder=2: a leashed chase.
        const sim::UnitType* sword = reg.find("armpw");
        check(sword && sword->defaultMove == 1 && sword->defaultFire == 2 && sword->leash == 640.0f,
              "armpw (StandingMoveOrder=1) defaults to a chase leashed at its maneuverleashlength 640");
        // StandingFireOrder=0: holds fire.
        const sim::UnitType* sniper = reg.find("armsnipe");
        check(sniper && sniper->defaultFire == 0, "armsnipe (StandingFireOrder=0) defaults to holding fire");
        // No key at all -> 2/2, roam + fire at will (retail's parser default).
        const sim::UnitType* plain = reg.find("armmex");
        check(plain && plain->defaultMove == 2 && plain->defaultFire == 2,
              "armmex (no standing order keys) defaults to roam + fire at will (2/2)");

        // A hold-position unit shoots what comes to it but never walks over.
        if (holder && sword) {
            sim::World w; freshWorld(w);
            int a = w.spawn(holder, 1000, 2600, 0, 0);
            check(w.unit(a)->moveState == 0 && w.unit(a)->fireState == 2,
                  "the spawned Commander carries its type's standing orders");
            float far = float(holder->maxRange()) + 120.0f;   // seen (sight 450), out of reach
            int prey = w.spawn(sword, 1000 + far, 2600, 0, 1);
            w.setStance(prey, 2);
            float startX = w.unit(a)->x.toFloat();
            for (int i = 0; i < 30 * 8; ++i) w.tick(1.0f / 30.0f);
            const sim::Unit* au = w.unit(a);
            check(au && std::fabs(au->x.toFloat() - startX) < 24.0f,
                  "a hold-position Commander does NOT walk to an out-of-range enemy",
                  "moved " + std::to_string(au ? au->x.toFloat() - startX : 0.0f) + "px");

            // A leashed (Maneuver) unit chases an enemy that its idle scan found in
            // weapon range, but no further than maneuverleashlength from its post,
            // then walks back (TotalA.exe Standby 0x405fe0 -> scan 0x43b700 ->
            // 0x43b1f0, Attack_Chase 0x4034a0; docs/retail-weapon-scripts.md 8.1).
            // An enemy beyond weapon range draws no advance at all.
            sim::World w2; freshWorld(w2);
            int a2 = w2.spawn(sword, 1000, 2600, 0, 0);
            const float near = float(sword->maxRange()) - 40.0f;
            const int e2 = w2.spawn(sword, 1000 + near, 2600, 0, 1);
            w2.setStance(e2, 2);
            check(w2.unit(a2)->moveState == 1, "the Peewee is leashed");
            float sx2 = w2.unit(a2)->x.toFloat();
            for (int i = 0; i < 30 * 2; ++i) w2.tick(1.0f / 30.0f);
            w2.order(e2, 1000 + near + 3000, 2600, false);   // the enemy walks away
            float farthest = 0;
            for (int i = 0; i < 30 * 40 && w2.unit(a2); ++i) {
                w2.tick(1.0f / 30.0f);
                farthest = std::max(farthest, w2.unit(a2)->x.toFloat() - sx2);
            }
            check(farthest > 24.0f, "a leashed unit chases an in-range enemy that walks away",
                  "farthest " + std::to_string(farthest) + "px");
            check(farthest <= sword->leash + 48.0f, "the chase stops at the leash from the post",
                  "farthest " + std::to_string(farthest) + "px, leash " + std::to_string(sword->leash));
            check(w2.unit(a2) && std::fabs(w2.unit(a2)->x.toFloat() - sx2) < 48.0f,
                  "the leashed unit walks back to its post",
                  "ended " + std::to_string(w2.unit(a2) ? w2.unit(a2)->x.toFloat() - sx2 : 0.0f) + "px from it");

            // ...and stays put for one beyond it.
            sim::World w4; freshWorld(w4);
            int a4 = w4.spawn(sword, 1000, 2600, 0, 0);
            const int e4 = w4.spawn(sword, 1000 + sword->leash + 220.0f, 2600, 0, 1);
            w4.setStance(e4, 2);
            float sx4 = w4.unit(a4)->x.toFloat();
            for (int i = 0; i < 30 * 8; ++i) w4.tick(1.0f / 30.0f);
            check(w4.unit(a4) && std::fabs(w4.unit(a4)->x.toFloat() - sx4) < 24.0f,
                  "but will not break its leash for one beyond it",
                  "moved " + std::to_string(w4.unit(a4) ? w4.unit(a4)->x.toFloat() - sx4 : 0.0f) + "px");

            // Hold fire: no target is ever acquired.
            sim::World w3; freshWorld(w3);
            int a3 = w3.spawn(holder, 1000, 2600, 0, 0);
            w3.spawn(sword, 1000 + 60, 2600, 0, 1);   // right next to it
            w3.setStance(a3, 2);   // Hold Fire
            check(w3.unit(a3)->fireState == 0, "the hold-fire button writes fire=0");
            bool acquired = false;
            for (int i = 0; i < 30 * 5 && !acquired; ++i) {
                w3.tick(1.0f / 30.0f);
                const sim::Unit* u3 = w3.unit(a3);
                if (u3 && !u3->orders.empty() && u3->orders.front().targetId) acquired = true;
            }
            check(!acquired, "a hold-fire unit never auto-acquires, even point blank");
        }
    }

    // ---- VTOL_standby: an idle flyer finds somewhere to put down -------------
    if (want("orders")) {
        const sim::UnitType* fly = reg.find("armatlas");
        check(fly && fly->canFly && fly->vtolStandby,
              "armatlas parses defaultmissiontype = VTOL_standby");
        if (fly && fly->vtolStandby) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = hpi::findMap(vfs, "Lake Shore");   // needs open water
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            // Park it over open water. Retail's landing-site predicate 509400 has
            // no movement-class terrain test: an explored site is refused only for
            // features, yards, occupants, and the type's own depth/slope words.
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

    // ---- stance buttons: which types have them at all ---------------------
    // TA's FBI gates the buttons with mobilestandorders / firestandorders (flagsB
    // bits 0/1, docs/retail-data.md 2.2); Kingdoms used unitstandorders. A plain
    // structure carries neither key (armsolar); the Atlas transport sets both to 0.
    if (want("orders")) {
        const sim::UnitType* solar = reg.find("armsolar");
        const sim::UnitType* atlas = reg.find("armatlas");
        const sim::UnitType* pw = reg.find("armpw");
        check(solar && !solar->canSetStance, "the Solar Collector has no combat stance");
        check(atlas && !atlas->canSetStance, "nor does the Atlas (mobilestandorders=0, firestandorders=0)");
        check(pw && pw->canSetStance, "an ordinary Peewee does");
        // The sim refuses to set one, not just the HUD -- a modified client must
        // not be able to hand a stance to something retail would not offer it for.
        if (atlas) {
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, reg, cfg);
            int c = w.spawn(atlas, 1000, 1000, 0, 0);
            int before = w.unit(c)->stance;
            w.setStance(c, before == 0 ? 2 : 0);
            check(w.unit(c)->stance == before, "and the sim refuses to change it");
        }
    }

    // ---- moves and builds queue as ONE sequence ------------------------------
    // Builds used to live in a separate list, so "go here, build that, go there"
    // ran as "go here, go there, (never build)" -- the build sat in the other
    // queue with nothing left to drain it.
    if (want("orders")) {
        sim::TypeRegistry& breg = reg;
        const sim::UnitType* mon = breg.find("armca");   // Construction Aircraft; a flyer
        const auto& menu = breg.buildable("armca");
        check(mon && mon->isBuilder && !menu.empty(), "armca is a builder with a menu");
        if (mon && !menu.empty()) {
            // A small structure from its menu: the Radar Tower (2x2). Not the
            // Dragon's Teeth: isfeature=1 turns those into a feature when done.
            const sim::UnitType* bt = breg.find("armrad");
            check(bt && std::find(menu.begin(), menu.end(), std::string("armrad")) != menu.end(),
                  "armca builds Radar Towers");
            if (!bt) bt = breg.find(menu[0]);
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, breg, cfg);
            w.setStartResources(0, 100000, 100000);
            float ox = 0, oz = 0;
            check(openApron(w, bt, 30, ox, oz), "open ground for the build queue");
            int id = w.spawn(mon, ox, oz + 100, 0, 0);
            w.order(id, ox + 300, oz + 100, false);             // move
            w.queueBuild(id, bt, ox + 360, oz + 200, true);     // ...then build
            w.order(id, ox + 600, oz + 100, true);              // ...then move again
            check(w.unit(id)->orders.size() == 3,
                  "move + build + move is ONE queue of three",
                  std::to_string(w.unit(id)->orders.size()) + " entries");

            bool built = false, sawSite = false;
            float furthest = 0;   // after the build: an idle flyer may then land a little way off
            for (int i = 0; i < 30 * 60; ++i) {
                w.tick(1.0f / 30.0f);
                const sim::Unit* u = w.unit(id);
                if (!u) break;
                if (u->buildSiteId != 0) sawSite = true;
                else if (sawSite) furthest = std::max(furthest, u->x.toFloat());
            }
            for (const auto& un : w.units())
                if (un.alive() && un.type == bt && !un.underConstruction) built = true;
            check(sawSite, "the queued build actually starts");
            check(built, "and finishes");
            const sim::Unit* u = w.unit(id);
            check(u && furthest > ox + 550.0f,
                  "and THEN the builder goes on to the move queued behind it",
                  u ? "reached x=" + std::to_string(int(furthest)) : "gone");
            check(u && u->orders.empty(),
                  "with nothing left stuck in the queue");

            // And the pure-build chain: three buildings, no movement between them.
            // Pinned BEFORE the queues were merged, so the merge cannot quietly
            // break the case that already worked.
            sim::World w2;
            sim::setupMatch(w2, breg, cfg);
            w2.setStartResources(0, 200000, 200000);
            int m2 = w2.spawn(mon, ox, oz + 100, 0, 0);
            const float sites[3][2] = {{ox + 200, oz + 200}, {ox + 320, oz + 200}, {ox + 440, oz + 200}};
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
            // Not "orders empty": armca is a flyer, and once the last building
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
        sim::TypeRegistry& breg = reg;
        const sim::UnitType* mon = breg.find("armca");
        const auto& menu = breg.buildable("armca");
        if (mon && !menu.empty()) {
            const sim::UnitType* bt = breg.find("armrad");
            if (!bt) bt = breg.find(menu[0]);
            sim::World w;
            sim::MatchConfig cfg;
            cfg.vfs = &vfs; cfg.mapPath = kMap;
            cfg.slots = {sim::MatchSlot{}, sim::MatchSlot{}};
            cfg.slots[0].team = 0; cfg.slots[1].team = 1;
            sim::setupMatch(w, breg, cfg);
            w.setStartResources(0, 200000, 200000);
            // The builder starts beside the first reclaimable feature of the map
            // and takes the next two within 600px of it.
            std::vector<int> feats;
            float fx = 0, fz = 0;
            for (const auto& f : w.features()) {
                if (!w.featureReclaimable(f)) continue;
                if (feats.empty()) { fx = f.x.toFloat(); fz = f.z.toFloat(); }
                float dx = f.x.toFloat() - fx, dz = f.z.toFloat() - fz;
                if (dx * dx + dz * dz < 600 * 600) feats.push_back(f.id);
                if (feats.size() >= 3) break;
            }
            float bx = fx, bz = fz;
            openApron(w, bt, 4, bx, bz, std::max(8, int(fx) / 16 - 30), std::max(8, int(fz) / 16 - 30));
            int id = w.spawn(mon, bx, bz, 0, 0);
            check(feats.size() == 3, "three reclaimable features near the builder");
            if (feats.size() == 3) {
                // A build first, then the area reclaim queued behind it -- the exact
                // shape that used to leave the reclaims parked forever, because the
                // old reclaimQueue only ever drained from inside tickReclaim and so
                // never started unless a reclaim was ALREADY running.
                w.queueBuild(id, bt, bx + 24, bz + 24, false);
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
    // Ctrl+D arms a countdown (selfdestructcountdown, 5 s when absent:
    // docs/retail-data.md 2.2) whose expiry zeroes hp. The death sweep runs
    // EARLIER in the same per-unit tick than the expiry, and healtime regen runs
    // AFTER it, so a regenerating unit (in TA: the Commanders, healtime=27)
    // could heal straight back off zero and survive.
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
                int id = w.spawn(c.t, 1000, 1000, 0, 0);
                if (id <= 0 || !w.unit(id)) continue;
                w.destroy(id);
                check(w.unit(id)->selfDestructT > 0.0f, (std::string(c.label) + ": countdown armed").c_str());
                tick(w, 8.0f);   // 5s countdown + slack
                const sim::Unit* u = w.unit(id);
                check(!u || !u->alive(), (std::string(c.label) + " (" + c.t->id + ") is DEAD").c_str(),
                      u ? ("hp=" + std::to_string(u->hp.toFloat())) : "gone");
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
                // A small unit, so its own blast leaves the scenery alone.
                const sim::UnitType* fader = reg.find("armpw");
                float fx = 0, fz = 0;
                openApron(w, fader, 30, fx, fz);
                const size_t feats0 = w.features().size();
                int id = w.spawn(fader, fx + 240, fz + 240, 0, 0);
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
            int id = w.spawn(regen, 1000, 1000, 0, 0);
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
    // not walk into. Measured on a Kingdoms map with 8 players before the snap: 7.2% of a
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
                cfg.slots[size_t(i)].faction = i % 2;   // ARM / CORE
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
                cfg.slots[size_t(i)].faction = i % 2;   // ARM / CORE
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
            // fought for 60s first, and the plan is capped at what the map can hold.
            check(total > 150, "the benchmark plan actually spawned an army",
                  std::to_string(total) + " units");
            check(total && bad * 100 / total < 3,
                  "benchmark units start on terrain they can occupy",
                  std::to_string(bad) + "/" + std::to_string(total) + " misplaced");
        }
    }

    // ---- per-unit damage overrides are ABSOLUTE ------------------------------
    //
    // TA's weapon [DAMAGE] block is `default=` plus `<unitname>=n` entries, and
    // the damage to a unit is map[targetUnitname] if present, else the default
    // (docs/retail-data.md 3, 0x499cd0): the entry IS the damage, not a scale.
    // (Kingdoms read its per-category entries as multipliers; that test and the
    // Crusades Barracks siege that motivated it have no TA counterpart.)
    if (want("orders")) {
        std::printf("\n[per-unit damage overrides]\n");
        sim::UnitType tgt{};
        tgt.id = "armthund";
        tgt.damageCategory = "armthund";
        sim::Weapon w{};
        w.damage = 360;
        check(w.damageVs(&tgt) == 360.0f, "no override -> the default damage");
        w.dmgVs["armthund"] = 97;
        check(w.damageVs(&tgt) == 97.0f, "an override is the damage itself",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs["armthund"] = 0;
        check(w.damageVs(&tgt) == 0.0f, "a 0 override means immune",
              std::to_string(w.damageVs(&tgt)));
        w.dmgVs.clear();
        w.dmgVs["armbrawl"] = 90;      // a unit this target is not
        check(w.damageVs(&tgt) == 360.0f, "an override for another unit is ignored");
        // The shipped data: the Pelican's missiles (armamph_weapon2) list
        // ARMTHUND=97 among their overrides.
        const sim::UnitType* pelican = reg.find("armamph");
        const sim::UnitType* thunder = reg.find("armthund");
        const sim::UnitType* peewee = reg.find("armpw");
        if (pelican && thunder && peewee && pelican->weapons.size() > 1) {
            const sim::Weapon& missiles = pelican->weapons[1];
            check(missiles.damageVs(thunder) == 97.0f, "armamph_weapon2 does its authored 97 to a Thunder",
                  std::to_string(missiles.damageVs(thunder)));
            check(missiles.damageVs(peewee) == float(missiles.damage),
                  "and its default to a unit it does not list",
                  std::to_string(missiles.damageVs(peewee)) + " vs " + std::to_string(missiles.damage));
        }
    }

    // ---- canMove is not a structure test ------------------------------------
    //
    // TA's factories (the Kbot Lab, every plant and shipyard) declare canmove=1
    // with no velocity. bmcode (0 = building) is the authoritative mobility flag,
    // so such a type must load as a structure that does not move.
    if (want("orders")) {
        std::printf("\n[canMove is not a structure test]\n");
        int factories = 0, wrong = 0;
        for (const auto& [id, t] : reg.types())
            if (t.classicFbi.bmcode == 0 && t.maxVel <= sim::Fixed() && !t.canFly) {
                ++factories;
                if (!t.isStructure() || t.canMove) ++wrong;
            }
        check(factories > 0 && wrong == 0, "every bmcode-0 type loads as a non-moving structure",
              std::to_string(wrong) + " of " + std::to_string(factories) + " wrong");
        const sim::UnitType* lab = reg.find("armlab");
        check(lab && lab->isStructure() && !lab->canMove,
              "the Kbot Lab (canmove=1 in its FBI) is one, and isStructure() answers for it");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
