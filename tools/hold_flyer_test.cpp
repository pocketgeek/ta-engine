// A flyer on Hold Position does not leave its post to attack what it finds,
// and an idle one does not fire from it either.
//   hold_flyer_test <data> [trace]
//
// TotalA.exe's VTOL_Standby (0x40f7d0) makes every weapon slot manual at its
// stage 0 (0x40f9d3: 0x4898b0(3) clears each slot record's automatic bit
// 0x10, 0x4898f4) and never makes them automatic again: an idle flyer's
// weapons fire only at what an attack mission hands them. Its stage 1 scans
// (0x43b700, on Fire At Will only: unit +0x110 & 0x300000 == 0x200000,
// 0x43b70a) and offers what it finds to 0x43b1f0(unit, found, 0), the call
// that puts the attack in; it refuses on Hold Position (+0x110 & 0xc0000 ==
// 0 without its force argument, 0x43b211), so no attack mission is pushed
// and nothing fires. (Kingdoms' standby left the slots automatic, so its
// flyers shot from their posts; World's Hold Position lift built on that.)
//
// Every armed flyer in the shipped data, on Hold Position, fire at will,
// with a seen enemy inside its weapon range (450 px when the weapon reaches
// it): no attack order, the flyer stays at its post and fires no shot. On
// Maneuver the same flyer takes the attack (the case that shows the test can
// fail). A flyer whose weapon reaches the air is also tried against a
// hovering enemy, which is AirToAir's (0x412d40) attack when ordered.
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
            check(!hold.attacked,label+": no attack order on Hold Position (0x43b1f0 refuses the standby's call)");
            check(hold.drift<=24.f,label+": stays at its post on Hold Position");
            check(hold.shots==0 && !hold.slotTook,label+": fires nothing from its post (VTOL_Standby's slots are manual)");
        }
    }
    std::printf("hold_flyer: %d cases, %d held, %d fired from their post, %d took the attack on Maneuver\n",
                cases,held,firedFromPost,maneuverTook);
    // The control: on Maneuver the standby's call goes through, so the
    // attack the Hold case must not get is there to be got.
    check(cases>=8,"enough armed flyers in the shipped data");
    check(maneuverTook*2>=cases,"on Maneuver most flyers take the attack (the test can fail)");
    // A landed hover attacker (the Brawler gunship, HoverAttack=1) with an
    // enemy 200 px behind it: on Hold Position it stays down and fires
    // nothing; on Maneuver the standby's scan sends it after the enemy, and
    // the attack's take-off lifts it.
    for (const uint8_t moveState:{uint8_t(0),uint8_t(1)}) {
        const sim::UnitType* brawler=registry.find("armbrawl");
        if (!brawler) { check(false,"armbrawl loads");break; }
        sim::World w;
        w.setSerialThreads(true);w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(512*512,70),512,512,64);
        w.buildNavClasses(registry);w.setPathService(true);
        w.setPowerCodes(true);w.applyPowerCode(0,sim::PowerCode::NowISee,sim::PowerArg::None,0);
        const int a=w.spawn(brawler,4096,4096,0,0);
        w.unit(a)->moveState=moveState;w.unit(a)->fireState=2;
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
        std::printf("  armbrawl %s, enemy behind: took off %d, hurt %d, drift %.1f\n",moveState ? "Maneuver" : "Hold",
                    int(flew),int(hurt),drift);
        if (moveState==0)
            check(landed && !flew && !hurt && w.unit(a)->orders.empty(),
                  "a landed Brawler on Hold Position stays down and fires nothing");
        else
            check(landed && flew && hurt,"on Maneuver it takes off and hits the enemy (the test can fail)");
    }
    std::printf("hold_flyer: %d failures\n",failures);
    return failures?1:0;
}
