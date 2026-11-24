// Landing must not turn ordinary flyers into powered-down combat units.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/retailhweffectdata.h"
#include "sim/retailaim.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <optional>
#include <bit>

namespace ta::sim { struct RetailReplayProbe {
    static void capture(World& w,int id,int owner) {w.captureUnit(*w.unit(id),owner);}
    static void impact(World& w,const Weapon& weapon,int from,int target) {
        const auto* source=w.unit(from);auto* victim=w.unit(target);
        w.applyHit(weapon,victim->x.toFloat(),victim->z.toFloat(),source->player,from,victim);
    }
    // An impact at a 16.16 point, as a projectile's collision reports it.
    static void blast(World& w,const Weapon& weapon,std::array<int32_t,3> point,int player,int struck=0) {
        w.applyHit(weapon,Fixed::raw(point[0]).toFloat(),Fixed::raw(point[2]).toFloat(),player,0,
                   struck ? w.unit(struck) : nullptr,point);
    }
    static uint32_t moveRate(World& w,int id) {
        const auto* script=w.unitScript(id);
        return script ? script->movementRate : ~0u;
    }
}; }

// A switcher takes an order only when its selected weapon can serve it:
// select the first weapon that hurts the target, as a player would.
static void selectWeaponFor(ta::sim::World& w,int unit,int target) {
    auto* u=w.unit(unit);const auto* t=w.unit(target);
    if (!u || !t || !u->type->weaponSwitching) return;
    for (size_t s=0;s<u->type->weapons.size() && s<3;++s)
        if (u->type->weapons[s].damageVs(t->type)>0) { u->weaponSlot=int(s);return; }
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        if (!ok) ++failures;
    };
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        // A Roach (corroach: kamikaze, no weapon) keeps its authored death
        // explosion (explodeas CRAWL_BLASTSML): killed beside an enemy, it
        // hurts it once Dying ends. (Its ATTACK_KAMIKAZE run is not ported.)
        {
            const auto* rat=registry.find("corroach");
            auto victim=*registry.find("armpw");victim.maxHp=25000;victim.healTime=0;
            victim.maxVel={};victim.weapon.damage=0;victim.weapons.clear();
            ta::sim::World world;
            world.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);world.buildNavClasses(registry);
            const int from=world.spawn(rat,1000,1000,0,0),target=world.spawn(&victim,1040,1000,0,1);
            world.tick(1.f/30);
            world.unit(from)->hp={};world.tick(1.f/30);
            for(int tick=0;tick<600 && world.unit(from)->deathPending;++tick)world.tick(1.f/30);
            check(world.unit(target)->hp<ta::sim::Fixed::fromInt(victim.maxHp),
                  "a weaponless Roach retains its authored death explosion");
        }

        {
            ta::sim::World world;
            world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            world.buildNavClasses(registry);
            const auto* type=registry.find("armfig");
            const int id=world.spawn(type,1000,1000,0,0);
            world.order(id,1800,1600,false);
            bool leaned=false,retained=false;
            for(int tick=0;tick<120;++tick) {
                world.tick(1.f/30.f);
                const auto& unit=*world.unit(id);
                leaned=leaned || unit.groundRoll!=0 || unit.groundPitch!=0;
                retained=retained || unit.flightAcceleration!=std::array<int32_t,3>{};
            }
            check(leaned && retained,"a flying fighter gets authoritative acceleration-driven attitude");
        }
        // TA's ordinary line-of-sight shots (Kbots, vehicles, a hovercraft):
        // a slot 0 the engine classes as plain straight.
        for(const char* name:{"armpw","armflash","armrock","corstorm","armmav","armanac"}) {
            const auto* definition=registry.find(name);
            if(!definition || definition->weapons.empty() ||
               !(definition->weapons[0].straight || definition->weapons[0].hasTaFlag(20))) {
                check(false,"straight roster definition exists with a straight slot 0");continue;
            }
            auto type=*definition;
            const bool naval=type.minWaterDepth>0;
            auto victim=*registry.find(naval ? "corpt" : "corak");victim.maxHp=25000;victim.healTime=0;
            victim.maxVel={};victim.weapon.damage=0;victim.weapons.clear();
            ta::sim::World world;
            world.setTerrain(std::vector<uint8_t>(128*128,naval ? 20 : 100),128,128,naval ? 100 : 20);
            world.buildNavClasses(registry);
            const int from=world.spawn(&type,1000,1000,0,0),target=world.spawn(&victim,1180,1000,0,1);
            world.setStartResources(0,100000,100000);world.attack(from,target,false);
            bool launched=false,hit=false,moved=false;
            for(int tick=0;tick<900 && !hit;++tick) {
                world.tick(1.f/30.f);
                // A selfprop rocket (armrock, corstorm) flies TotalA.exe's
                // selfprop motion, which takes precedence over the
                // line-of-sight one (docs/retail-data.md 3.3, rule 1).
                for(const auto& shot:world.projectiles())if(shot.fromId==from && (shot.straight || shot.taSelfProp)) {
                    launched=true;moved=moved || shot.age>0;
                }
                hit=world.unit(target)->hp<ta::sim::Fixed::fromInt(victim.maxHp) || world.unit(target)->incapacitated();
            }
            std::printf("straight balance=%s unit=%s launched=%d moved=%d hit=%d hash=%016llx\n","standard",name,int(launched),int(moved),int(hit),
                static_cast<unsigned long long>(world.stateHash()));
            check(launched && moved && hit,"shipped ordinary LOS weapon flies and hits through live combat");
        }
        std::vector<std::string> flyers;
        for (const auto& [id,type]:registry.types())
            if (type.canFly && !type.onOffable && type.weapon.damage>0) flyers.push_back(id);
        std::sort(flyers.begin(),flyers.end());
        for (const auto& name:flyers) for (int order=0;order<3;++order) {
            ta::sim::World world;
            world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            world.buildNavClasses(registry);
            const auto* type=registry.find(name);
            if (!type) { ++failures;continue; }
            int flyer=world.spawn(type,1000,1000,0,0);
            world.order(flyer,1200,1000,false);
            // Exercise the actual VTOL standby/landing controller, not a manually
            // cleared active flag: this is what made already-landed flyers fail.
            for (int tick=0;tick<1800 && world.unit(flyer)->active;++tick)
                world.tick(1.f/30.f);
            if (type->vtolStandby)
                check(!world.unit(flyer)->active,"landing runs the retail deactivation callback");
            for (int tick=0;tick<300;++tick) world.tick(1.f/30.f);
            auto victim=*registry.find("corak");victim.maxHp=30000;victim.healTime=0;victim.weapon.damage=0;victim.weapons.clear();
            const float x=world.unit(flyer)->x.toFloat(),z=world.unit(flyer)->z.toFloat();
            const float distance=order==0 ? std::min(200.f,float(type->weapon.range)*0.5f) : 1000.f;
            victim.maxVel={};
            world.setStartResources(0,100000,100000);
            int target=world.spawn(&victim,x+distance,z,0,1);
            if (order==1) world.attackMove(flyer,x+distance,z,false);
            if (order==2) selectWeaponFor(world,flyer,target);
            if (order==2) world.attack(flyer,target,false);
            bool fired=false,attacked=false,seen=false;
            for (int tick=0;tick<1200 && !fired;++tick) {
                world.tick(1.f/30.f);
                fired=world.unit(flyer)->justFired;
                const auto& o=world.unit(flyer)->orders;
                attacked=attacked || (!o.empty() && o.front().targetId==target);
                seen=seen || world.playerSees(0,*world.unit(target));
            }
            std::printf("balance=%s unit=%s order=%d fired=%d attacked=%d seen=%d\n","standard",
                        name.c_str(),order,int(fired),int(attacked),int(seen));
            // TA's bombs are commandfire (never swept or picked; only an
            // explicit order fires them) and its torpedoes waterweapon: a
            // flyer with no other weapon leaves a ground enemy alone unless
            // ordered to attack it (the Thunder, Shadow, Lancet, Titan).
            bool autoWeapon=false;
            for (const auto& weapon:type->weapons) autoWeapon|=!weapon.commandFire && !weapon.waterWeapon && !weapon.toAir;
            if (order<2 && !autoWeapon) {
                check(!attacked && !fired,"a flyer with only command-fire or water weapons leaves a ground enemy alone");
                continue;
            }
            // Idle on Hold Position (standingunitorder 1), VTOL_Standby's
            // 4d7de0(unit, found, 0, 0, 1, 0) refuses: the flyer stays landed
            // and only its sweep slots may take the target.
            if (order==0 && world.unit(flyer)->moveState==0) {
                check(!attacked,"an idle flyer on Hold Position puts no attack in");
                continue;
            }
            // Its scan and sweep take only an enemy its player sees: a landed
            // flyer whose sight falls short of the target leaves it alone.
            if (order==0 && !seen) {
                check(!attacked && !fired,"an idle flyer leaves an enemy its player cannot see");
                continue;
            }
            // A very short authored maneuver leash intentionally refuses this
            // idle target. Explicit orders override it.
            if (order==0 && world.unit(flyer)->moveState==1 && type->leash>0 && distance>type->leash) {
                check(!fired,"automatic attack respects the authored maneuver leash");
                continue;
            }
            // A fight-move's pick is the move's own scan (4d8370), which runs
            // the picker on the scanning slot alone (519ac0: a switcher's
            // selected one): a scanning slot that does nothing to the target
            // leaves it, as retail's would.
            // The stride scan it used to rely on weighed every weapon.
            if (order==1 && !type->weapons.empty() && type->weapons[0].damageVs(&victim)<=0) {
                check(!attacked,"a fight-move leaves a target its scanning slot cannot hurt");
                continue;
            }
            check(fired,"landed flyer can acquire, Move Fight, and directly attack");
        }
        // Range alone is not the hover attack destination. Keep targets alive so
        // these checks cover settling, backing away, altitude and target pursuit.
        for (const auto& name:flyers) {
            auto type=*registry.find(name);
            if (!type.hoverAttack && !type.weapon.hoverAttack) continue;
            for (bool close:{false,true}) {
                ta::sim::World w;
                // (Kingdoms' weapon-level hoverattackdistance/altitude override
                // case is gone: no TA weapon carries those keys.)
                const bool overrides=false;
                const int ground=overrides?240:100;
                w.setTerrain(std::vector<uint8_t>(256*256,overrides?230:100),256,256,overrides?240:20);
                auto victim=*registry.find("corak");victim.maxHp=30000;   // Fixed is 16.16: anything above 32767 overflows (UB; wraps on Linux, saturates to a corpse on MinGW)
                victim.healTime=0;
                victim.weapon.damage=0;victim.weapons.clear();
                for (auto& weapon:type.weapons) { weapon.mindControl=false;weapon.damage=1; }
                if (overrides) {
                    type.weapons.front().hoverAttackDistance=88;
                    type.weapons.front().hoverAttackAltitude=300;
                }
                type.weapon=type.weapons.front();
                const int a=w.spawn(&type,1500,1500,0,0);
                const int b=w.spawn(&victim,close?1520:2500,1500,0,1);
                w.setStartResources(0,100000,100000);w.attack(a,b,false);
                for (int tick=0;tick<1500;++tick) w.tick(1.f/30.f);
                const auto* u=w.unit(a);const auto* t=w.unit(b);
                const auto& weapon=type.weapons.front();
                const int distance=weapon.hoverAttackDistance?weapon.hoverAttackDistance:type.hoverAttackDistance;
                const int altitude=weapon.hoverAttackAltitude?weapon.hoverAttackAltitude:type.hoverAttackAltitude;
                const float radius=ta::sim::fxLen(u->x-t->x,u->z-t->z).toFloat();
                std::printf("hover unit=%s close=%d distance=%.2f wanted=%d height=%.2f wanted=%d\n",
                    name.c_str(),int(close),radius,distance,u->flightY.toFloat(),std::min(511,ground+altitude));
                check(std::abs(radius-distance)<20,"hover flyer settles at authored attack distance");
                check(std::abs(u->flightY.toFloat()-std::min(511,ground+altitude))<2,
                      "hover flyer uses attack altitude and remains airborne");
                check(u->flightGroundMode==2 && !u->orders.empty(),"hover arrival preserves attack order");
                const float previousZ=t->z.toFloat();
                w.order(b,t->x.toFloat(),previousZ+150,false);
                for (int tick=0;tick<900;++tick) w.tick(1.f/30.f);
                u=w.unit(a);t=w.unit(b);
                check(t->z.toFloat()>previousZ+100,"pursuit fixture target actually moved");
                check(std::abs(ta::sim::fxLen(u->x-t->x,u->z-t->z).toFloat()-distance)<20,
                      "hover flyer follows a moving target");
            }
        }
        // A flyer without hover attack does not stop to shoot a target that
        // cannot fly: its mission, AirToGround (41bf80), flies attack runs. It
        // closes to attackrunlength/2 of the target (half its range without
        // one), then flies on past it, along its own line through the target,
        // attackrunlength + 128..255 px (3 x its range without one), turns 90
        // degrees and comes round again. World held it still in range. An
        // A.K. beside the target shoots at it as it crosses (reported).
        for (const auto& name:flyers) {
            auto type=*registry.find(name);
            if (type.hoverAttack || type.weapon.hoverAttack) continue;
            for (auto& weapon:type.weapons) { weapon.mindControl=false;weapon.damage=1;weapon.aoe=0; }
            type.weapon=type.weapons.front();
            type.maxHp=30000;type.healTime=0;   // Fixed is 16.16: keep hp below 32768
            auto dummy=*registry.find("corak");dummy.maxHp=30000;dummy.healTime=0;
            dummy.maxVel={};dummy.weapon.damage=0;dummy.weapons.clear();
            const auto& shooter=*registry.find("corak");
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int ground=w.spawn(&dummy,2300,2000,0,0),flyer=w.spawn(&type,2300,2600,0,1);
            const int gunner=w.spawn(&shooter,2150,2000,0,0);
            selectWeaponFor(w,flyer,ground);
            w.attack(flyer,ground,false);
            const float range=type.weapons.front().range;
            const auto* g=w.unit(ground);
            int held=0,runs=0,onLine=0,shots=0,hits=0;bool near=false,fired=false;
            std::optional<ta::sim::RetailFlightGoal> last;
            for (int tick=0;tick<1800;++tick) {
                if (tick==120) w.attack(gunner,flyer,false);   // WEAPON1, the A.K.'s laser
                const auto* before=w.unit(flyer);
                const int32_t x=before->x.v,y=before->flightY.v,z=before->z.v;
                w.tick(1.f/30.f);
                const auto* u=w.unit(flyer);
                if (u->orders.empty() || u->orders.front().targetId!=ground) break;
                if (u->flightGroundMode==2 && u->x.v==x && u->flightY.v==y && u->z.v==z) ++held;
                near=near || ta::sim::fxLen(u->x-g->x,u->z-g->z).toFloat()<=range/2;
                fired=fired || u->firedWeapons;
                if (w.unit(gunner)->firedWeapons&1) ++shots;
                for (const auto& hit:w.hits())
                    if (hit.weapon==&shooter.weapons.front() && hit.victimId==flyer) ++hits;
                const auto& order=u->orders.front();
                const auto& goal=order.flightGoal;
                const bool installed=goal && (!last || last->point.x!=goal->point.x || last->point.z!=goal->point.z);
                if (installed && (order.mission.stage==5 || order.mission.stage==6) && goal->radius>=128) {
                    // A run point: from where the flyer stood, through the target.
                    ++runs;
                    const double gx=(goal->point.x-g->x.v)/65536.0,gz=(goal->point.z-g->z.v)/65536.0;
                    const double fx=(g->x.v-x)/65536.0,fz=(g->z.v-z)/65536.0;
                    const double past=std::hypot(gx,gz);
                    const double lo=type.attackRunLength ? type.attackRunLength+128 : 3*range;
                    const double hi=type.attackRunLength ? type.attackRunLength+255 : 3*range;
                    const double turn=std::abs(std::atan2(gx*fz-gz*fx,gx*fx+gz*fz));
                    onLine+=past>=lo-3 && past<=hi+3 && turn<0.035 && goal->radius<=255;
                }
                if (goal) last=*goal; else last.reset();
            }
            std::printf("attack runs balance=%s unit=%s runlength=%d range=%.0f held=%d near=%d runs=%d on line=%d fired=%d"
                " A.K. shots=%d hits=%d\n","standard",name.c_str(),int(type.attackRunLength),range,
                held,int(near),runs,onLine,int(fired),shots,hits);
            check(held==0,"a flyer attacking a ground target never holds still");
            check(near,"its runs carry it within half its range of the target");
            // A bomber's slot 0 is Dropped: its run points come from the bomb
            // run (a 960 px goal radius, points 200..970 px past the target),
            // not the attackrunlength window measured here (Kingdoms 41bf80;
            // TotalA.exe's bomber run is not yet specified in docs/retail-*).
            const bool bomber=type.weapons.front().kind==ta::sim::Weapon::Kind::Dropped;
            check(runs>0 && (bomber || onLine==runs),"every run point lies past the target on the flyer's line through it");
            check(fired,"it fires during its runs");
        }
        // Engine deviation (owner's call 2026-11-15): a flyer on attack runs
        // re-aims each script-timed release (World::fire). Retail launches
        // from the angles its readiness gate froze about 15 ticks earlier, so
        // a closing flyer's shot landed long and a turning one's wide
        // (docs/retail-engine.md "Attack runs", measured on the Kingdoms
        // Gryphon). Real weapons against a dummy topped up every tick:
        // shots, direct hits and damage are reported for each, and most of
        // a Hawk's (armhawk, Guided Missiles) shots must strike it.
        for (const auto& name:flyers) {
            auto type=*registry.find(name);
            if (type.hoverAttack || type.weapon.hoverAttack) continue;
            type.maxHp=30000;type.healTime=0;
            auto dummy=*registry.find("corak");dummy.maxHp=30000;dummy.healTime=0;
            dummy.maxVel={};dummy.weapon.damage=0;dummy.weapons.clear();
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int ground=w.spawn(&dummy,2300,2000,0,0),flyer=w.spawn(&type,2300,2600,0,1);
            selectWeaponFor(w,flyer,ground);
            w.attack(flyer,ground,false);
            const auto* hp=w.unit(ground);
            int shots=0,hits=0;double damage=0;
            for (int tick=0;tick<3600;++tick) {
                w.unit(ground)->hp=ta::sim::Fixed::fromInt(dummy.maxHp);
                w.tick(1.f/30.f);
                const auto* u=w.unit(flyer);
                if (!u || u->orders.empty() || u->orders.front().targetId!=ground) break;
                shots+=std::popcount(u->firedWeapons);
                damage+=dummy.maxHp-hp->hp.toFloat();
                for (const auto& hit:w.hits())
                    if (hit.victimId==ground && hit.weapon>=type.weapons.data() &&
                        hit.weapon<type.weapons.data()+type.weapons.size()) ++hits;
            }
            std::printf("attack-run aim balance=%s unit=%s shots=%d hits=%d damage=%.0f\n","standard",name.c_str(),shots,hits,damage);
            if (name=="armhawk")
                check(shots>=8 && hits*2>shots,"a Hawk on attack runs strikes the target with most of its shots");
        }
        // Retail turns the body toward the point a slot aims at (52fe30): the
        // led SweetSpot (51aa50) for every class but melee. A pivoting ground
        // unit's mover turns to that request at rest, and 52fff0 gates on it;
        // World faced the target's centre. A Rocko (armrock, range 400) given
        // an instant turn (a TA Kbot's turnrate, ~1000 a tick, never catches a
        // crossing flyer's request) reaches its request every tick, and it moves first in each tick, so
        // its heading after a tick is the request made from the positions and
        // velocity read before it. Not before its weapon aims, though: after
        // a walk Attack_Chase gives the slot its target two ticks after the
        // stop, and the body faces from the tick after (World::tickCombat).
        // Those held ticks are not counted.
        auto nativeFrom=[](const std::array<int32_t,3>& origin,const std::array<int32_t,3>& point) {
            using ta::sim::Fixed;
            return uint16_t(ta::sim::retailDirection(
                Fixed::raw(std::bit_cast<int32_t>(uint32_t(origin[0])-uint32_t(point[0]))),
                Fixed::raw(std::bit_cast<int32_t>(uint32_t(origin[2])-uint32_t(point[2])))).v);
        };
        {
            auto flyerType=*registry.find("armpeep");flyerType.maxHp=30000;flyerType.healTime=0;
            flyerType.weapons.clear();flyerType.weapon={};
            auto gunner=*registry.find("armrock");
            gunner.turnRate=gunner.turnInPlaceRate=32767;
            const auto& gunnerWeapon=gunner.weapons.front();
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int shooter=w.spawn(&gunner,2000,2000,0,0),flyer=w.spawn(&flyerType,2300,1500,0,1);
            w.patrol(flyer,2300,2500);   // crossing the A.K.'s front at 300 px
            for (int tick=0;tick<240;++tick) w.tick(1.f/30.f);
            w.attack(shooter,flyer,false);
            int aiming=0,led=0,centre=0;
            for (int tick=0;tick<900;++tick) {
                const auto* s=w.unit(shooter);const auto* f=w.unit(flyer);
                const std::array<int32_t,3> origin{s->x.v,s->groundY.v,s->z.v},middle{f->x.v,f->flightY.v,f->z.v};
                const auto lead=ta::retailAimLead(origin,w.queryUnitScriptPoint(flyer,true),
                    {f->flightVelocity.x,f->flightVelocity.y,f->flightVelocity.z},
                    ta::retailAimSpeed(gunnerWeapon.projVel),gunnerWeapon.noLead);
                const uint16_t toLead=nativeFrom(origin,lead),toCentre=nativeFrom(origin,middle);
                const bool rest=s->speed==ta::sim::Fixed();
                const uint32_t simulated=w.tickCount();
                w.tick(1.f/30.f);
                s=w.unit(shooter);
                if (!rest || s->speed!=ta::sim::Fixed() ||
                    ta::sim::fxLen(f->x-s->x,f->z-s->z).toFloat()>gunnerWeapon.range) continue;
                if (!s->orders.empty() && simulated<=s->orders.front().attackAimTick) continue;
                ++aiming;
                const uint16_t heading=ta::sim::portHeadingToRetail(s->heading);
                led+=heading==toLead;centre+=heading==toCentre;
            }
            std::printf("facing balance=%s armrock vs crossing armpeep aiming=%d facing led=%d centre=%d\n","standard",aiming,led,centre);
            check(aiming>100 && led>=aiming-2,"a pivoting shooter faces the led SweetSpot of a moving flyer");
        }
        // A HOVERATTACK flyer's target controller (4e4140, flags 0xa3) heads it
        // at the target's SweetSpot, unled (4e4330), and every mission wake
        // draws the ring around the selected weapon's led SweetSpot (41de3f ->
        // 51aa50). An Arm Shipyard's (armsy) SweetSpot sits 37 px off its
        // centre, so the two rings are told apart by radius. The Brawler
        // (armbrawl) is the hover attacker.
        {
            auto hoverer=*registry.find("armbrawl");hoverer.maxHp=30000;hoverer.healTime=0;
            for (auto& weapon:hoverer.weapons) { weapon.damage=1;weapon.dmgVs.clear();weapon.dmgVsIds.clear();weapon.aoe=0; }
            hoverer.weapon=hoverer.weapons.front();
            auto yard=*registry.find("armsy");yard.maxHp=30000;yard.healTime=0;
            yard.weapons.clear();yard.weapon={};
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int target=w.spawn(&yard,2000,2000,0,0),flyer=w.spawn(&hoverer,2000,2600,0,1);
            w.attack(flyer,target,false);
            const auto* b=w.unit(target);
            const std::array<int32_t,3> middle{b->x.v,b->groundY.v,b->z.v};
            const auto sweetSpot=w.queryUnitScriptPoint(target,true);
            int ticks=0,headed=0,refreshes=0,ringed=0;
            ta::sim::RetailFlightVector last{};
            for (int tick=0;tick<1200;++tick) {
                const auto* f=w.unit(flyer);
                const std::array<int32_t,3> from{f->x.v,f->flightY.v,f->z.v};
                w.tick(1.f/30.f);
                f=w.unit(flyer);
                // AirToGroundHover's ring: the target controller (flag 1), not
                // the stage-1 approach point.
                if (f->orders.empty() || !f->orders.front().flightGoal || !(f->orders.front().flightGoal->flags&1)) continue;
                const auto& order=f->orders.front();
                ++ticks;
                headed+=order.flightGoal->heading==nativeFrom(from,sweetSpot);
                const auto& point=order.flightGoal->point;
                if (point.x==last.x && point.z==last.z) continue;
                last=point;++refreshes;
                const float radius=std::hypot(float(point.x-sweetSpot[0])/65536.f,float(point.z-sweetSpot[2])/65536.f);
                // Within 16 px, plus the ring point's fixed-point truncation.
                ringed+=std::abs(radius-float(hoverer.hoverAttackDistance))<=16.05f;
            }
            const float offset=std::hypot(float(sweetSpot[0]-middle[0])/65536.f,float(sweetSpot[2]-middle[2])/65536.f);
            std::printf("hover controller balance=%s armbrawl vs armsy (SweetSpot %.1f px off) ticks=%d headed=%d"
                " refreshes=%d ringed=%d\n","standard",offset,ticks,headed,refreshes,ringed);
            check(offset>16 && ticks>1000 && headed==ticks,"a hover attacker heads at its target's SweetSpot");
            check(refreshes>20 && ringed==refreshes,"a hover attacker rings the led SweetSpot at its hover distance");
        }
        // World's other flyer holds skip the flight body the same way, and none
        // stopped both. SetMission's wait ("w N") and ambush ("wa") and FlyingBuild waiting out mobile units on
        // its site kept the velocity the flyer came in with and only braked its
        // speed; an emptied order list (until standby lands it) zeroed the
        // velocity but only braked the speed; a flying builder at its repair or
        // reclaim point kept a velocity with no speed. Shooters lead by the
        // velocity (51aa50), the COB speed queries and MoveRate read both, and
        // the next flight step starts from them, so a held flyer reports neither.
        auto still=[](const ta::sim::Unit& u,int32_t x,int32_t y,int32_t z) {
            return u.flightGroundMode==2 && u.x.v==x && u.flightY.v==y && u.z.v==z;
        };
        auto reportHold=[&](ta::sim::World& w,int id,const char* hold,bool held,bool turning) {
            const auto& u=*w.unit(id);
            const auto& v=u.flightVelocity;
            const uint32_t rate=ta::sim::RetailReplayProbe::moveRate(w,id);
            std::printf("hold balance=%s unit=%s hold=%s held=%d velocity=(%d,%d,%d) speed=%d rate=%u\n","standard",u.type->id.c_str(),hold,int(held),v.x,v.y,v.z,u.speed.v,rate);
            check(held,"the flyer holds still");
            check(v.x==0 && v.y==0 && v.z==0 && u.speed==ta::sim::Fixed(),"a held flyer reports no velocity or speed");
            if (!turning) check(rate==0,"a held flyer that is not turning reports MoveRate 0");
        };
        for (const char* name:{"armfig","armthund"}) for (int hold=0;hold<4;++hold) {
            static const char* const holds[]={"wait","ambush","queued wait","stop"};
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int flyer=w.spawn(registry.find(name),1000,1000,0,0);
            w.order(flyer,hold==2 ? 1300.f : 3000.f,1000,false);
            // The queued wait starts where VTOL_Move retires: on its coarse
            // arrival, still flying, because another order follows.
            if (hold==2) w.orderWait(flyer,10,true);
            auto waiting=[&] { const auto& o=w.unit(flyer)->orders; return !o.empty() && o.front().wait>0; };
            for (int tick=0;tick<(hold==2 ? 600 : 120) && !(hold==2 && waiting());++tick) w.tick(1.f/30.f);
            if (hold==0) w.orderWait(flyer,10,false);
            if (hold==1) w.orderWaitAttack(flyer,false);
            if (hold==3) w.stop(flyer);
            bool held=false;
            for (int tick=0;tick<1 && !held;++tick) {
                const auto* u=w.unit(flyer);
                const int32_t x=u->x.v,y=u->flightY.v,z=u->z.v;
                w.tick(1.f/30.f);
                held=still(*w.unit(flyer),x,y,z);
            }
            reportHold(w,flyer,holds[hold],held,false);
            if (hold==3) check(!w.unit(flyer)->moving(),"a flyer stopped with no orders is not moving");
        }
        // A guard used to hold within 70 px of its charge too. Retail's
        // VTOL_Follow (0x418860) never holds: it circles the guarded unit at
        // its weapon range + 160 (320 px unarmed), so the flyer keeps flying.
        for (const char* name:{"armfig","armthund"}) {
            ta::sim::World w;
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const int charge=w.spawn(registry.find("armpw"),1600,1000,0,0);
            const int flyer=w.spawn(registry.find(name),1000,1000,0,0);
            w.guard(flyer,charge,false);
            const auto* type=w.unit(flyer)->type;
            const float ring=type->weapons.empty() ? 320.f : float(type->weapons.front().range+160);
            int stillTicks=0;float far=0;
            for (int tick=0;tick<900;++tick) {
                const auto* u=w.unit(flyer);
                const int32_t x=u->x.v,y=u->flightY.v,z=u->z.v;
                w.tick(1.f/30.f);
                if (tick<300) continue;
                stillTicks+=still(*w.unit(flyer),x,y,z);
                far=std::max(far,std::hypot((w.unit(flyer)->x-w.unit(charge)->x).toFloat(),
                                            (w.unit(flyer)->z-w.unit(charge)->z).toFloat()));
            }
            std::printf("guard balance=%s unit=%s ring=%.0f still=%d farthest=%.0f\n","standard",name,ring,stillTicks,far);
            check(stillTicks==0 && far<ring+60,"a guarding flyer circles its charge instead of holding");
        }
        for (int hold=0;hold<3;++hold) {
            static const char* const holds[]={"repair","reclaim","placement retry"};
            ta::sim::World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            ta::sim::FeatType rock;rock.reclaimable=true;w.setFeatureTypes({rock});
            w.setStartResources(0,100000,100000);
            const int flyer=w.spawn(registry.find("armca"),1000,1000,0,0);
            w.order(flyer,1050,1000,false);
            for (int tick=0;tick<60;++tick) w.tick(1.f/30.f);
            auto patient=*registry.find("corak");patient.maxVel={};patient.healTime=0;
            auto blocker=*registry.find("corak");blocker.weapons.clear();blocker.weapon.damage=0;
            if (hold==0) {
                const int id=w.spawn(&patient,1400,1000,0,0);
                w.unit(id)->hp=ta::sim::Fixed::fromInt(patient.maxHp/2);
                w.repair(flyer,id,false);
            }
            if (hold==1) { w.addFeature(4242,1400,1000,50,1,1,true,0);w.reclaim(flyer,4242,false); }
            if (hold==2) {
                w.queueBuild(flyer,registry.find("armdrag"),1400,1000,false);
                w.spawn(&blocker,1400,1000,0,1);   // another player's unit on the site
            }
            bool held=false;
            for (int tick=0;tick<600 && !held;++tick) {
                const auto* u=w.unit(flyer);
                const int32_t x=u->x.v,y=u->flightY.v,z=u->z.v;
                const bool refused=!u->orders.empty() && u->orders.front().placementFailures>0;
                w.tick(1.f/30.f);
                held=still(*w.unit(flyer),x,y,z) && (hold!=2 || refused);
            }
            // A working builder pivots toward its job, so MoveRate may be 1.
            reportHold(w,flyer,holds[hold],held,true);
        }
        // Retail's impact dispatch (529c10): a weapon with areaofeffect <= 16
        // that struck a unit hits it alone (52a460); every other impact damages
        // units by the distance from the 3D impact to their bounding boxes
        // (529dc0). World measured flat distance to unit centres, so a shell
        // bursting on the ground hurt a flyer cruising over it.
        {
            auto weaponNamed=[&](const char* unit,const char* name) -> const ta::sim::Weapon* {
                for (const auto& weapon:registry.find(unit)->weapons) if (weapon.name==name) return &weapon;
                return nullptr;
            };
            const auto* shell=weaponNamed("armham","Plasma Cannon");   // the Hammer's (aoe 48)
            // The Peewee's E.M.G. (aoe 8) widened to 16, the largest direct-hit
            // area: no TA weapon has an areaofeffect between 9 and 16, and a
            // 4 px radius would leave the grazing case below nothing to hit.
            std::optional<ta::sim::Weapon> emg;
            if (const auto* peewee=weaponNamed("armpw","E.M.G.")) { emg=*peewee;emg->aoe=16; }
            const auto* swirl=emg ? &*emg : nullptr;
            check(shell && shell->aoe>16 && swirl && swirl->aoe>0 && swirl->aoe<=16,
                  "splash fixtures: a blast and a small-area weapon");
            if (shell && swirl) {
                using ta::sim::Fixed;
                // A one-cell dummy, so the bodies placed 3 and 20 px from the centre one
                // do not overlap (an A.K. is 2x2).
                auto dummy=*registry.find("corak");dummy.maxHp=30000;dummy.healTime=0;
                dummy.maxVel={};dummy.weapon.damage=0;dummy.weapons.clear();dummy.footX=dummy.footZ=1;
                auto flyerType=*registry.find("armfig");flyerType.maxHp=30000;flyerType.healTime=0;
                ta::sim::World w;w.setPlayerCount(2);
                w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
                w.buildNavClasses(registry);
                ta::sim::FeatType tree;tree.name="tree";tree.hp=100000;w.setFeatureTypes({tree});
                const int32_t ground=100*65536;
                auto loss=[&](int id,Fixed before) { return (before-w.unit(id)->hp).floorInt(); };
                // retail's fraction for a unit `gap` px from the blast
                auto fraction=[](const ta::sim::Weapon& weapon,int gap) {
                    const double t=double(gap)/double(weapon.aoe/2)-1.0;
                    return gap ? float(t*t*(1.0-double(weapon.edge))+double(weapon.edge)) : 1.0f;
                };
                auto spread=[](int dealt,float scaled) {
                    return dealt>=int(scaled*0.85f)-1 && dealt<=int(scaled*1.15f)+1;
                };
                const int flyer=w.spawn(&flyerType,1000,1000,0,1);
                w.order(flyer,3000,1000,false);
                for (int tick=0;tick<150;++tick) w.tick(1.f/30.f);
                const auto* f=w.unit(flyer);
                const int altitude=(f->flightY.v-ground)>>16;
                check(f->flightGroundMode==2 && altitude>shell->aoe/2,"fixture flyer cruises above the blast radius");
                Fixed hp=f->hp;
                ta::sim::RetailReplayProbe::blast(w,*shell,{f->x.v,ground,f->z.v},0);
                const int under=loss(flyer,hp);
                hp=w.unit(flyer)->hp;
                ta::sim::RetailReplayProbe::blast(w,*shell,{f->x.v,f->flightY.v,f->z.v},0);
                const int burst=loss(flyer,hp);
                // The side dummy stands west of the blast: 529dc0's window
                // stops one cell short on the +x side (retailSplashWindow).
                const int centre=w.spawn(&dummy,1500,1500,0,1),side=w.spawn(&dummy,1480,1500,0,1);
                w.tick(1.f/30.f);   // World's old splash read the grid of the last tick
                const Fixed full=w.unit(centre)->hp;
                ta::sim::RetailReplayProbe::blast(w,*shell,{1500*65536,ground,1500*65536},0);
                const int centreLoss=loss(centre,full),sideLoss=loss(side,full);
                const int gap=20-8*dummy.footX;
                const float base=shell->damageVs(&dummy);
                // The E.M.G. strikes the centre dummy with another standing in its radius.
                const int close=w.spawn(&dummy,1503,1500,0,1),edgeUnit=w.spawn(&dummy,1620+8*dummy.footX,1500,0,1);
                w.addFeature(93*256+93,1500,1500,0,1,1,false,0);
                w.tick(1.f/30.f);
                Fixed centreHp=w.unit(centre)->hp;
                ta::sim::RetailReplayProbe::blast(w,*swirl,{1500*65536,ground,1500*65536},0,centre);
                const int struck=loss(centre,centreHp),bystander=loss(close,full);
                const int scenery=w.feature(93*256+93) ? w.feature(93*256+93)->dmg : -1;
                // ...and the ground 4 px from a unit's box, with nobody struck.
                ta::sim::RetailReplayProbe::blast(w,*swirl,{1616*65536,ground,1500*65536},0);
                const int grazed=loss(edgeUnit,full);
                std::printf("splash balance=%s flyer altitude=%d under=%d burst=%d dummy centre=%d side=%d"
                    " (%d px from its box, fraction %.4f of %.0f) swirl struck=%d bystander=%d scenery=%d grazed=%d\n","standard",altitude,under,burst,centreLoss,sideLoss,gap,
                    fraction(*shell,gap),base,struck,bystander,scenery,grazed);
                check(under==0,"a ground blast misses a flyer cruising over it");
                check(burst>0 && spread(burst,shell->damageVs(&flyerType)),"a blast at the flyer's height hits it in full");
                check(spread(centreLoss,base),"a blast on a ground unit still hits it in full");
                check(spread(sideLoss,base*fraction(*shell,gap)),"splash falls off with the distance to the unit's box");
                check(struck>0,"a small-area weapon hits the unit it struck");
                check(bystander==0 && scenery==0,"...and nothing else: no splash and no scenery");
                check(spread(grazed,swirl->damageVs(&dummy)*fraction(*swirl,4)),
                      "a small-area weapon striking the ground hurts a unit within its radius");
            }
        }
        // Preserve the intentional off switch for units that actually support it.
        auto turret=*registry.find("armstump");turret.onOffable=true;
        auto victim=*registry.find("corak");victim.maxHp=30000;victim.healTime=0;victim.weapon.damage=0;victim.weapons.clear();
        ta::sim::World world;
        world.setTerrain(std::vector<uint8_t>(128*128,100),128,128,20);
        int a=world.spawn(&turret,800,800,1.570796327f,0),b=world.spawn(&victim,950,800,0,1);
        world.setActive(a,false);world.attack(a,b,false);
        for (int tick=0;tick<300;++tick) world.tick(1.f/30.f);
        check(world.unit(b)->hp.toFloat()==victim.maxHp,"powered-down on/off units still cannot fire");
        world.setActive(a,true);
        for (int tick=0;tick<300;++tick) world.tick(1.f/30.f);
        check(world.unit(b)->hp.toFloat()<victim.maxHp,"powered-up units resume firing");
    }
    return failures?1:0;
}
