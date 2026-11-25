// VTOL_SeekAttack (TotalA.exe 0x4103e0): an attack flyer whose target is lost circles the
// target's last point and, on Fire At Will, attacks the next enemy that comes
// near. tools/re/check_air_seek_attack.py compares the handler and the
// lost-target exits (41c8f6, 41d654) with the original executable; this
// checks World runs them: AirToGround's and AirToAir's kills push the seek,
// the flyer orbits at slot 0's weapon range + 160 px and keeps flying,
// a new enemy at the point is attacked on Fire At Will and not on Return
// Fire, and no seek follows on Hold Fire, behind a queued order or after an
// idle flyer's own auto attack (which sits on its default mission).
//
//   air_seek_attack_test <retail-install-dir>
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;

// A target that dies to the first hits and does not fight back.
UnitType fragile(const UnitType& type) {
    UnitType t=type;
    t.maxHp=1;t.healTime=0;t.maxVel={};
    t.weapons.clear();t.weapon={};
    return t;
}
// A ground unit that cannot die and does not fight back.
UnitType sturdy(const UnitType& type) {
    UnitType t=type;
    t.maxHp=30000;t.healTime=0;t.maxVel={};
    t.weapons.clear();t.weapon={};
    return t;
}

enum class Setup { Explicit, Queued, Idle };

struct Run {
    bool killed=false,seek=false,attacksNew=false,landedSeeking=false,leftSeek=false;
    int killTick=-1,hops=0,points=0;
    float pointError=1e9f,nearest=1e9f,farthest=0,radius=0;
};

// `attacker` kills a fragile `victim` (a flyer when `air`) and is then
// watched for `after` ticks; a sturdy enemy appears at the victim's point
// `spawnAt` ticks after the kill (never when negative).
Run run(const TypeRegistry& registry,const UnitType& attacker,const UnitType& victim,int fireState,Setup setup,
        int after,int spawnAt) {
    Run r;
    World w;
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.buildNavClasses(registry);
    
    const auto weak=fragile(victim);
    const auto bystander=sturdy(*registry.find("armpw"));
    const float vx=2000,vz=2000;
    const int them=w.spawn(&weak,vx,vz,0.f,1);
    w.unit(them)->standbyAllowed=false;
    w.setStance(them,2);w.unit(them)->moveState=0;w.unit(them)->fireState=0;
    // An idle flyer stands inside its first weapon's range (at most 150 px
    // off: a Kingdoms flyer with a 140 px weapon never took a target from
    // 150, unseen while its attack runs never killed and it was skipped,
    // before flyers re-aimed at the release).
    const auto& weapons=attacker.weapons;
    const float idleGap=std::min(150.f,0.75f*float(weapons.front().range));
    const int me=w.spawn(&attacker,vx,setup==Setup::Idle ? vz+idleGap : vz+600,0.f,0);
    w.unit(me)->fireState=uint8_t(fireState);
    w.unit(me)->moveState=2;   // Roam: some flyers' FBI holds position, and 4d7de0 refuses those
    if (setup!=Setup::Idle) w.attack(me,them,false);
    if (setup==Setup::Queued) w.order(me,vx+900,vz+900,true);
    r.radius=float(weapons.front().range+160);   // slot 0's range (0x41063e)
    float lastX=vx,lastZ=vz;
    int fresh=0;
    for (int tick=0;tick<1800 && !r.killed;++tick) {
        if (const Unit* t=w.unit(them);t && t->alive() && t->hp>Fixed()) { lastX=t->x.toFloat();lastZ=t->z.toFloat(); }
        w.tick(kTick);
        const Unit* t=w.unit(them);
        if (!t || !t->alive() || t->hp<=Fixed()) { r.killed=true;r.killTick=tick; }
    }
    if (!r.killed) return r;
    std::set<uint16_t> angles;
    std::set<std::pair<int32_t,int32_t>> points;
    for (int tick=0;tick<after;++tick) {
        if (tick==spawnAt) {
            // TotalA.exe's VTOL_SeekAttack (0x4103e0) orbits at slot 0's range
            // + 160 (0x41063e..0x410651) but scans, at each wake, only within
            // its own sightdistance (0x4105fa -> 0x43b700 -> 0x40b7b0, arg 3
            // zero: type +0x202). A bomber's orbit (range 1280) never comes
            // near the point, so its new enemy waits at the orbit point it is
            // flying to, where the controller's arrival wakes the scan.
            float sx=lastX+40,sz=lastZ;
            const Unit* u=w.unit(me);
            if (r.radius>float(attacker.sight) && u && !u->orders.empty() && u->orders.front().seekAttack &&
                u->orders.front().flightGoal) {
                sx=u->orders.front().flightGoal->point.x/65536.f+40;
                sz=u->orders.front().flightGoal->point.z/65536.f;
            }
            fresh=w.spawn(&bystander,sx,sz,0.f,1);
        }
        w.tick(kTick);
        const Unit* u=w.unit(me);
        if (!u || u->orders.empty()) { if (r.seek) r.leftSeek=true; continue; }
        const auto& o=u->orders.front();
        if (fresh && o.targetId==fresh) r.attacksNew=true;
        if (!o.seekAttack) { if (r.seek) r.leftSeek=true; continue; }
        if (!r.seek) r.pointError=std::hypot(o.x.toFloat()-lastX,o.z.toFloat()-lastZ);
        r.seek=true;
        if (u->flightGroundMode!=2) r.landedSeeking=true;
        angles.insert(o.guardOrbitAngle);
        if (o.flightGoal) points.insert({o.flightGoal->point.x,o.flightGoal->point.z});
        // Once it has reached its first orbit point, how far it strays.
        if (points.size()>1) {
            const float d=std::hypot(u->x.toFloat()-o.x.toFloat(),u->z.toFloat()-o.z.toFloat());
            r.nearest=std::min(r.nearest,d);r.farthest=std::max(r.farthest,d);
        }
    }
    r.hops=int(angles.size())-1;r.points=int(points.size());
    return r;
}

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    int failures=0;
    auto check=[&](bool ok,const char* label,const std::string& what) {
        std::printf("[%s] %s: %s\n",ok?"PASS":"FAIL",what.c_str(),label);
        if (!ok) ++failures;
    };
    {
        TypeRegistry registry;setupRegistry(registry,vfs);
        const std::string balance="standard";
        // Armed flyers that fly attack runs (AirToGround: no hoverattack).
        std::vector<std::string> flyers;
        for (const auto& [id,type]:registry.types())
            if (type.canFly && !type.onOffable && type.weapon.damage>0 && !type.hoverAttack &&
                !type.weapon.hoverAttack && !type.weapons.empty())
                flyers.push_back(id);
        std::sort(flyers.begin(),flyers.end());
        const auto& ground=*registry.find("armpw");
        int tested=0;
        for (const auto& name:flyers) {
            const auto& type=*registry.find(name);
            const std::string what=balance+" "+name;
            // AirToGround, Fire At Will: the kill, the orbit, the new enemy.
            const Run fire=run(registry,type,ground,2,Setup::Explicit,2100,1200);
            if (!fire.killed) { std::printf("[SKIP] %s: no kill\n",what.c_str()); continue; }
            ++tested;
            std::printf("%s fire-at-will: kill=%d point error=%.1f hops=%d points=%d orbit %.0f..%.0f (radius %.0f)"
                        " attacks new=%d\n",what.c_str(),fire.killTick,fire.pointError,fire.hops,fire.points,
                        fire.nearest,fire.farthest,fire.radius,int(fire.attacksNew));
            check(fire.seek,"the kill pushes VTOL_SeekAttack",what);
            check(fire.pointError<1,"it circles the target's last point",what);
            check(fire.hops>=2 && fire.points>=3,"it hops round the point",what);
            check(fire.farthest<fire.radius+400 && fire.nearest<fire.radius,"it orbits at range + 160",what);
            check(!fire.landedSeeking,"it keeps flying",what);
            check(fire.attacksNew,"on Fire At Will it attacks a new enemy at the point",what);
            // The attack's own slot (slot 0, as Attack_Chase gives it) must reach a
            // land target for the Return Fire / Hold Fire kills: a TA seaplane's slots
            // 0 and 1 are torpedoes (waterweapon=1) and its slot 2 is automatic, which
            // only Fire At Will lets take the target (armseap: no kill in 1800 ticks).
            if (type.weapons.front().waterWeapon) {
                std::printf("[SKIP] %s: its attack slot is a torpedo; the land kill is its automatic slot's\n",
                            what.c_str());
            } else {
                // Return Fire: it orbits but does not go looking.
                const Run back=run(registry,type,ground,1,Setup::Explicit,2100,1200);
                check(back.killed && back.seek && !back.leftSeek && !back.attacksNew,
                      "on Return Fire it seeks but does not retarget",what);
                // Hold Fire: the attack just ends.
                const Run hold=run(registry,type,ground,0,Setup::Explicit,600,-1);
                check(hold.killed && !hold.seek,"on Hold Fire no seek follows",what);
            }
            // A queued order behind the attack: the attack was not the last mission.
            const Run queued=run(registry,type,ground,2,Setup::Queued,600,-1);
            check(queued.killed && !queued.seek,"behind a queued order no seek follows",what);
            // An idle flyer's own auto attack sits on its default mission.
            const Run idle=run(registry,type,ground,2,Setup::Idle,600,-1);
            check(idle.killed && !idle.seek,"an idle flyer's auto attack ends without a seek",what);
        }
        check(tested>=3,"enough flyers made a kill",balance);
        // AirToAir: a flyer killing a flyer seeks round the target's last point
        // (TotalA.exe's AirToAir never writes mission +0x22).
        int airTested=0;
        for (const auto& name:flyers) {
            const auto& type=*registry.find(name);
            const std::string what=balance+" "+name+" vs armpeep";
            const Run air=run(registry,type,*registry.find("armpeep"),2,Setup::Explicit,600,-1);
            if (!air.killed) continue;
            ++airTested;
            std::printf("%s air-to-air: kill=%d point error=%.1f hops=%d\n",what.c_str(),air.killTick,air.pointError,
                        air.hops);
            check(air.seek && air.pointError<64,"an AirToAir kill pushes the seek at the target's last point",what);
        }
        check(airTested>=3,"enough flyers made an air kill",balance);
    }
    return failures?1:0;
}
