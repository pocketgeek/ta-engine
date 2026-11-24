// A flyer on Hold Position does not leave its post to attack what it finds.
//   hold_flyer_test <data> [trace]
//
// Retail's VTOL_Standby scans (4d8370, 4176bc) and offers what it finds to
// 4d7de0(unit, found, 0, 0, 1, 0) (4176cc), the call that inserts the auto
// attack. 4d7de0 refuses it on Hold Position: (+0x130 & 0x30000) == 0 with
// neither explicit (arg 3) nor the forced return (arg 6) set returns 0
// (4d7e0a..4d7e19), so no attack mission is pushed and the standby goes on
// (417736). What is left is the fire-at-will sweep (40f330): the auto slots
// take a target in the weapon's own range and fire from where the flyer is.
//
// Every armed flyer in the shipped data, on Hold Position, fire at will,
// with a seen enemy inside its weapon range (450 px when the weapon reaches
// it): no attack order and the flyer stays at its post, and most fire at it
// from there through their auto slots (before the fix every one of them
// that acquired it flew off 190-960 px after it). On Maneuver the same
// flyer takes the attack (the case that shows the test can fail). A flyer
// whose weapon reaches the air is also tried against a hovering enemy, which
// is AirToAir's (41ca50) attack when ordered.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/powercodes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace ta;
namespace {
int failures=0;
void check(bool ok,const std::string& label) {
    if (!ok) std::printf("FAIL %s\n",label.c_str());
    failures+=!ok;
}
struct Result { bool attacked=false; float drift=0; int shots=0; bool slotTook=false; };
Result runCase(const sim::TypeRegistry& registry,const sim::UnitType& flyer,bool airTarget,float dist,
               uint8_t moveState,bool trace,int ticks) {
    sim::World w;
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
    w.buildNavClasses(registry);
    w.setPathService(true);
    w.setStartResources(0,1e6f,1e6f);w.setStartResources(1,1e6f,1e6f);   // energypershot
    // The sweep takes only enemies its player sees (Player::visibleEnemies).
    w.setPowerCodes(true);
    w.applyPowerCode(0,sim::PowerCode::NowISee,sim::PowerArg::None,0);
    const int a=w.spawn(&flyer,4096,4096,1.5707963f,0);
    w.unit(a)->moveState=moveState;w.unit(a)->fireState=2;
    // Let it settle into its standby (landed, or hovering for one that
    // cannot land) before the enemy shows up.
    for (int i=0;i<90;++i) w.tick(1.f/30);
    const float postX=w.unit(a)->x.toFloat(),postZ=w.unit(a)->z.toFloat();
    sim::UnitType target=*registry.find(airTarget ? "armpeep" : "armpw");
    target.maxHp=30000;target.healTime=0;target.weapons.clear();target.weapon.damage=0;
    const int t=w.spawn(&target,postX+dist,postZ,0,1);
    w.setStance(t,2);w.unit(t)->moveState=0;w.unit(t)->fireState=0;
    if (airTarget) w.order(t,postX+dist,postZ+12,false);
    Result r;
    int lastReload[3]{};
    for (int tick=0;tick<ticks;++tick) {
        w.unit(t)->hp=sim::Fixed::fromInt(30000);
        if (airTarget && tick%30==29) w.order(t,postX+dist,postZ+((tick/30)&1 ? 12.f : -12.f),false);
        w.tick(1.f/30);
        const sim::Unit& u=*w.unit(a);
        for (const auto& o:u.orders) if (o.targetId==t) r.attacked=true;
        for (int s=0;s<3;++s) {
            // A shot sets the bit, or (for a scripted fire that reports
            // none) restarts the slot's reload.
            if ((u.firedWeapons>>s&1) || u.reloads[size_t(s)]>lastReload[s]) ++r.shots;
            lastReload[s]=u.reloads[size_t(s)];
            if (u.slotTarget[size_t(s)]==t) r.slotTook=true;
        }
        const float dx=u.x.toFloat()-postX,dz=u.z.toFloat()-postZ;
        r.drift=std::max(r.drift,std::sqrt(dx*dx+dz*dz));
        if (trace && tick%30==0)
            std::printf("  t=%3d pos=%.1f,%.1f y=%.1f mode=%d orders=%zu slot=%d fired=%x\n",tick,u.x.toFloat(),
                        u.z.toFloat(),u.flightY.toFloat(),int(u.flightGroundMode),u.orders.size(),
                        u.slotTarget[0],u.firedWeapons);
    }
    return r;
}
} // namespace
int main(int argc,char** argv) {
    if (argc<2) return 2;
    const bool trace=argc>2 && std::strcmp(argv[2],"trace")==0;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs);
    for (const char* id:{"armpeep","armpw"})
        if (!registry.find(id)) { std::printf("FAIL missing type %s\n",id);return 1; }
    std::vector<const sim::UnitType*> flyers;
    for (const auto& [id,type]:registry.types())
        if (type.canFly && !type.isStructure() && type.weapon.damage>0 && !type.weapons.empty())
            flyers.push_back(&type);
    int cases=0,maneuverTook=0,held=0,firedFromPost=0;
    for (const auto* f:flyers) {
        for (bool air:{false,true}) {
            const sim::Weapon& wp=f->weapons.front();
            if (wp.melee) continue;
            if (air && wp.noAir) continue;
            if (!air && (wp.toAir || wp.hasTaFlag(17))) continue;   // toairweapon
            const float range=float(f->maxRange());
            // Inside the standby's scan too: TotalA.exe's VTOL_Standby scan (4176bc ->
            // 4d8370 -> 0x43b700 -> picker 0x40b7b0) gathers enemies within the unit's
            // own sightdistance, 350 on every TA flyer (Kingdoms' were wider).
            const float dist=std::min(range>470.f ? 450.f : range*0.8f,0.8f*float(f->sight));
            if (trace) std::printf("%s %s at %.0f (range %.0f, hover %d)\n",f->id.c_str(),air?"air":"ground",
                                   dist,range,int(f->hoverAttack));
            const Result hold=runCase(registry,*f,air,dist,0,trace,450);
            const Result maneuver=runCase(registry,*f,air,dist,1,false,450);
            ++cases;
            maneuverTook+=maneuver.attacked;
            held+=!hold.attacked && hold.drift<=24.f;
            firedFromPost+=hold.shots>0;
            std::printf("  %-9s %-6s at %3.0f: Hold attack=%d drift=%.1f shots=%d slot=%d | Maneuver attack=%d drift=%.1f\n",
                        f->id.c_str(),air?"air":"ground",dist,int(hold.attacked),hold.drift,hold.shots,
                        int(hold.slotTook),int(maneuver.attacked),maneuver.drift);
            const std::string label=f->id+(air?" (air target)":" (ground target)");
            check(!hold.attacked,label+": no attack order on Hold Position (4d7de0 refuses the standby's call)");
            check(hold.drift<=24.f,label+": stays at its post on Hold Position");
        }
    }
    std::printf("hold_flyer: %d cases, %d held, %d fired from their post, %d took the attack on Maneuver\n",
                cases,held,firedFromPost,maneuverTook);
    // The control: on Maneuver the standby's call goes through, so the
    // attack the Hold case must not get is there to be got.
    check(cases>=8,"enough armed flyers in the shipped data");
    check(maneuverTook*2>=cases,"on Maneuver most flyers take the attack (the test can fail)");
    // Hold Position is not Hold Fire: the sweep's auto slots still shoot a
    // seen enemy in range from where the flyer stands by.
    check(firedFromPost*2>=cases,"on Hold Position most flyers fire from their post");
    // Engine deviation (owner, 2026-11-12, World::tickHoldLift): a landed
    // hover attacker (here the Brawler gunship, HoverAttack=1) on Hold
    // Position with an enemy behind it takes off, turns to it and hits it
    // from over its post, without chasing, and lands again once the enemy is
    // gone: a landed hover attacker cannot turn to a request. A Passive one
    // (Hold Fire) takes no target and stays down.
    // Straight ahead, facing it, it has no facing reason to lift: it hits
    // the enemy within 200 ticks, from the ground or once a shot it started
    // has gone unreleased for kHoldLiftNoRelease ticks.
    // (The TAK case of a hovering dropped-bomb flyer has no TA counterpart:
    // TA's bombers do not hover.)
    {
        const sim::UnitType* brawler=registry.find("armbrawl");
        sim::World w;
        w.setSerialThreads(true);w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
        w.buildNavClasses(registry);w.setPathService(true);
        w.setPowerCodes(true);w.applyPowerCode(0,sim::PowerCode::NowISee,sim::PowerArg::None,0);
        const int a=brawler ? w.spawn(brawler,4096,4096,0,0) : 0;
        if (a) {
            w.unit(a)->moveState=0;w.unit(a)->fireState=2;
            for (int i=0;i<90;++i) w.tick(1.f/30);
            sim::UnitType target=*registry.find("armpw");
            target.maxHp=30000;target.healTime=0;target.weapons.clear();target.weapon.damage=0;target.maxVel={};
            const int t=w.spawn(&target,w.unit(a)->x.toFloat(),w.unit(a)->z.toFloat()+192,0,1);
            const auto hp=w.unit(t)->hp;
            int first=-1;
            for (int i=0;i<600 && first<0;++i) { w.tick(1.f/30);if (w.unit(t)->hp<hp) first=i; }
            std::printf("  armbrawl Hold, enemy straight ahead: first hit at tick %d\n",first);
            check(first>=0 && first<200,"a landed Brawler facing an enemy hits it within 200 ticks");
        }
    }
    for (const uint8_t fireState:{uint8_t(2),uint8_t(0)}) {
        const sim::UnitType* brawler=registry.find("armbrawl");
        if (!brawler) { check(false,"armbrawl loads");break; }
        sim::World w;
        w.setSerialThreads(true);w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
        w.buildNavClasses(registry);w.setPathService(true);
        w.setPowerCodes(true);w.applyPowerCode(0,sim::PowerCode::NowISee,sim::PowerArg::None,0);
        const int a=w.spawn(brawler,4096,4096,0,0);
        w.unit(a)->moveState=0;w.unit(a)->fireState=fireState;
        for (int i=0;i<90;++i) w.tick(1.f/30);
        const bool landed=w.unit(a)->flightGroundMode==1;
        const float postX=w.unit(a)->x.toFloat(),postZ=w.unit(a)->z.toFloat();
        sim::UnitType target=*registry.find("armpw");
        target.maxHp=30000;target.healTime=0;target.weapons.clear();target.weapon.damage=0;target.maxVel={};
        const int t=w.spawn(&target,postX,postZ-200,0,1);   // behind it
        const auto hp=w.unit(t)->hp;
        bool flew=false;float drift=0;
        for (int i=0;i<600;++i) {
            w.tick(1.f/30);
            const auto& u=*w.unit(a);
            flew|=u.flightGroundMode==2;
            drift=std::max(drift,std::hypot(u.x.toFloat()-postX,u.z.toFloat()-postZ));
            if (trace && i%30==0)
                std::printf("  t=%3d heading=%d mode=%d y=%.1f slot=%d aim=%d fired=%x reload=%d\n",i,int(u.heading.v),
                            int(u.flightGroundMode),u.flightY.toFloat(),u.slotTarget[0],u.scriptAimTarget,
                            u.firedWeapons,u.reloads[0]);
        }
        const bool hurt=w.unit(t)->hp<hp;
        if (fireState==2) {
            std::printf("  armbrawl Hold, enemy behind: took off %d, hurt %d, drift %.1f\n",int(flew),int(hurt),drift);
            check(landed && flew && hurt,"a landed Brawler on Hold Position takes off and hits an enemy behind it");
            check(drift<=24.f && w.unit(a)->orders.empty(),"it does not chase: it stays over its post with no order");
            w.unit(t)->hp=sim::Fixed();
            for (int i=0;i<900;++i) w.tick(1.f/30);
            check(w.unit(a)->flightGroundMode==1,"it lands again once the enemy is gone");
        } else {
            check(landed && !flew && !hurt,"a Passive (Hold Fire) Brawler stays landed");
        }
    }
    std::printf("hold_flyer: %d failures\n",failures);
    return failures?1:0;
}
