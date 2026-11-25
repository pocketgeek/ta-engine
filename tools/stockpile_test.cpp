// TotalA.exe's stockpile weapons and interceptors.
//
// Stockpiles (weapon flag bit 28): the build menu's MAKENUKE button
// (0x419b00) gives a BuildWeapon mission (0x402b70) that stocks the slot one
// missile per reloadtime, paying metalpershot / energypershot five ticks at
// a time through the unit's own books (0x4011c0): stage 1's first step is
// paid at once, so a stock takes (ceil(reload/5) - 1) * 5 ticks. A shot
// needs stock (0x49e3e2), spends one (0x49e455), sets no reload and pays no
// storage (0x49e463 -> 0x49e4f2). Clicks join the last BuildWeapon mission
// on the slot and a right click takes one back (0x43b0b0).
//
// Interceptors (bits 30 / 29): the fire sweep (0x4089a0) gives an anti-nuke
// with stock the point of an enemy targetable shot whose target point is
// within its coverage (0x49d120); its vlaunch routine (0x49db70) launches a
// rocket that chases that shot (+0x56, 0x49b47a) and bursts within its
// areaofeffect of it (0x49b106..0x49b1a9), and the burst destroys every shot
// in that area (0x49a664..0x49a7ee).
//
// Checked: armsilo stocks a nuclear_missile in 5395 ticks for 180000 E and
// 2000 M, half of it by half time; two stocks in a row take 5395 each; the
// queued count follows the clicks and an unqueue removes the mission. With no
// stock an attack order fires nothing; with one it fires once, spends the
// stock, leaves storage alone and fires no more. An armamd with stock shoots
// down a nuke aimed inside its coverage (the target lives); one whose
// coverage does not hold the target point never fires (the target dies).
//
//   stockpile_test <retail-install-dir>
#include "hpi/hpi.h"
#include "net/lockstep.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;
int failures=0;
void check(bool ok,const std::string& what) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",what.c_str());
    if (!ok) ++failures;
}

void setup(World& w,const TypeRegistry& registry) {
    w.setSerialThreads(true);
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,60),256,256,20);
    w.buildNavClasses(registry);
    w.setStartResources(0,1e6f,1e6f);w.setStartResources(1,1e6f,1e6f);
}

const Projectile* nukeShot(const World& w,int silo) {
    for (const auto& p:w.projectiles()) if (p.fromId==silo && !p.spent && p.life>0) return &p;
    return nullptr;
}

} // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    for (const char* id:{"armsilo","armamd","armsolar"})
        if (!registry.find(id)) { std::printf("FAIL missing type %s\n",id);return 1; }
    const UnitType* silo=registry.find("armsilo");
    const Weapon& nuke=silo->weapons.at(0);
    check(nuke.hasTaFlag(28) && nuke.hasTaFlag(29) && nuke.reloadTicks==5400 &&
          nuke.energyPerShot==180000.f && nuke.metalPerShot==2000.f,
          "armsilo's nuclear_missile: stockpile, targetable, 5400 ticks, 180000 E / 2000 M");

    // Building stock.
    {
        World w;setup(w,registry);
        const int s=w.spawn(silo,1000,1000,0.f,0);
        w.tick(kTick);   // settled, idle
        const float e0=w.books(0).stored[0],m0=w.books(0).stored[1];
        // Through the command path, as the button sends it.
        ta::net::Command c;c.kind=ta::net::Cmd::Train;c.player=0;c.unitId=s;c.targetId=1;
        std::snprintf(c.type,sizeof c.type,"%s","MAKENUKE");
        applyCommand(w,registry,c);
        applyCommand(w,registry,c);
        check(w.queuedStock(*w.unit(s),0)==2 && w.unit(s)->orders.size()==1,
              "two clicks queue two stock on one BuildWeapon mission");
        const uint32_t start=w.tickCount();
        uint32_t first=0,second=0;
        float spentHalf=-1;
        for (int t=0;t<12000 && !second;++t) {
            w.tick(kTick);
            const Unit& u=*w.unit(s);
            if (!first && u.stock[0]==1) first=w.tickCount();
            if (first && !second && u.stock[0]==2) second=w.tickCount();
            if (w.tickCount()==start+2700+30) spentHalf=e0-w.books(0).stored[0];
        }
        std::printf("  stock 1 at +%u, stock 2 at +%u (gap %u), half-time spend %.0f E\n",
                    first-start,second-start,second-first,spentHalf);
        check(first && first-start>=5394 && first-start<=5396,
              "the first stock comes (ceil(5400/5) - 1) * 5 = 5395 ticks after the order");
        check(second && second-first==5395,"the next stock follows 5395 ticks later");
        check(spentHalf>85000 && spentHalf<95000,"the energy is paid gradually: about half by half time");
        for (int t=0;t<60;++t) w.tick(kTick);
        const float de=e0-w.books(0).stored[0],dm=m0-w.books(0).stored[1];
        std::printf("  spent %.1f E, %.1f M for two stock\n",de,dm);
        check(std::fabs(de-360000.f)<1.f && std::fabs(dm-4000.f)<1.f,
              "two stock cost exactly 2 x (180000 E, 2000 M)");
        check(w.unit(s)->orders.empty() && w.queuedStock(*w.unit(s),0)==0,"the mission ends with its count built");

        // Unqueue.
        applyCommand(w,registry,c);applyCommand(w,registry,c);applyCommand(w,registry,c);
        ta::net::Command un=c;un.kind=ta::net::Cmd::Unqueue;
        applyCommand(w,registry,un);
        check(w.queuedStock(*w.unit(s),0)==2,"a right click takes one queued stock back");
        un.targetId=5;applyCommand(w,registry,un);
        check(w.unit(s)->orders.empty(),"taking back more than is queued removes the mission");
        // Another unit's button refuses: no stockpile weapon.
        const int solar=w.spawn(registry.find("armsolar"),1400,1000,0.f,0);
        c.unitId=solar;applyCommand(w,registry,c);
        check(w.unit(solar)->orders.empty(),"a unit without a stockpile weapon takes no BuildWeapon");
    }

    // Firing only from stock.
    {
        World w;setup(w,registry);
        const int s=w.spawn(silo,600,600,0.f,0);
        const int target=w.spawn(registry.find("armsolar"),3400,3400,0.f,1);
        w.tick(kTick);
        w.attack(s,target,false);
        bool fired=false;
        for (int t=0;t<300;++t) { w.tick(kTick);if (nukeShot(w,s)) fired=true; }
        check(!fired,"with no stock the silo's attack fires nothing");
        w.unit(s)->stock[0]=1;
        const float e0=w.books(0).stored[0];
        int shots=0;bool wasFlying=false;
        for (int t=0;t<1500;++t) {
            w.tick(kTick);
            const bool flying=nukeShot(w,s)!=nullptr;
            if (flying && !wasFlying) ++shots;
            wasFlying=flying;
        }
        const Unit& u=*w.unit(s);
        std::printf("  shots %d, stock %d, reload %d, energy delta %.0f\n",shots,int(u.stock[0]),
                    u.reloads[0],e0-w.books(0).stored[0]);
        check(shots==1 && u.stock[0]==0,"with one stock it fires one nuke and spends the stock");
        check(u.reloads[0]==0,"a stockpile shot sets no reload");
        check(std::fabs(e0-w.books(0).stored[0])<1.f,"and takes nothing from storage");
        const Unit* t=w.unit(target);
        check(!t || !t->alive() || t->hp<Fixed::fromInt(t->type->maxHp),"the nuke reaches its target");
    }

    // Interceptors.
    struct Case { const char* label; float amdX,amdZ; bool intercepts; };
    for (const Case& k:{Case{"an armamd whose coverage holds the target point",3000,3000,true},
                        Case{"an armamd whose coverage does not",600,3400,false}}) {
        World w;setup(w,registry);
        const int s=w.spawn(silo,600,600,0.f,0);
        const int target=w.spawn(registry.find("armsolar"),3400,3400,0.f,1);
        const int amd=w.spawn(registry.find("armamd"),k.amdX,k.amdZ,0.f,1);
        w.tick(kTick);
        w.unit(s)->stock[0]=1;
        w.unit(amd)->stock[0]=1;
        w.attack(s,target,false);
        uint32_t chasedSerial=0,nukeSerial=0;
        bool rocket=false;
        for (int t=0;t<2000;++t) {
            w.tick(kTick);
            for (const auto& p:w.projectiles()) {
                if (p.fromId==s && !nukeSerial) nukeSerial=p.serial;
                if (p.fromId==amd) { rocket=true;chasedSerial=p.targetShot; }
            }
        }
        const Unit* t=w.unit(target);
        const bool targetWhole=t && t->alive() && t->hp>=Fixed::fromInt(t->type->maxHp);
        std::printf("  %s: rocket %d chasing %u (nuke %u), amd stock %d, target whole %d\n",k.label,int(rocket),
                    chasedSerial,nukeSerial,int(w.unit(amd)->stock[0]),int(targetWhole));
        if (k.intercepts) {
            check(rocket && chasedSerial && chasedSerial==nukeSerial,
                  std::string(k.label)+": launches a rocket that chases the nuke");
            check(w.unit(amd)->stock[0]==0,std::string(k.label)+": spends its stock");
            check(targetWhole,std::string(k.label)+": the nuke is destroyed before it lands");
        } else {
            check(!rocket && w.unit(amd)->stock[0]==1,std::string(k.label)+": never fires");
            check(!targetWhole,std::string(k.label)+": the nuke lands");
        }
    }

    std::printf("%s (%d failure%s)\n",failures?"FAILED":"OK",failures,failures==1?"":"s");
    return failures?1:0;
}
