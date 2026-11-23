// A melee weapon cannot reach a unit in the air (icd, emulated 2026-10-30;
// tools/re/check_melee_air.py).
//
// MeleeWeapon's in-range test (52b980) answers false for a target whose
// +0x130 low bits are 2 (airborne, our flightGroundMode) before it looks at
// the footprints, so the weapon update never aims or swings at a flyer. The
// target picker (412620) keeps only grounded units (layer 1) for a melee
// slot, and so do its keep test and the hit response (409a50). An ordered
// attack still gets Attack_Melee (4de530 has no air test for it), but
// Attack_Melee's head (404781..4047ee) ends the mission at its first
// dispatch while the target is airborne. A flyer that has landed (layer 1) is
// an ordinary melee target. World tested footprint contact in 2D only: a
// Titan stood under a held Gryphon and hammered it, and an idle one acquired
// a flyer and walked under it; the sweep skipped every flyer type, landed too.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using tak::sim::Fixed;
using tak::sim::UnitType;
using tak::sim::World;

int failures=0;
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",label.c_str());
    if (!ok) ++failures;
}

// The weapon 519ac0 hands the attack mission: a switcher's selected slot,
// otherwise the first slot.
int meleeSlot(const UnitType& t) {
    if (t.weapons.empty()) return -1;
    if (t.weaponSwitching) {
        for (size_t s=0;s<t.weapons.size() && s<3;++s) if (t.weapons[s].melee) return int(s);
        return -1;
    }
    return t.weapons.front().melee ? 0 : -1;
}

UnitType target(const tak::sim::TypeRegistry& registry,const char* name) {
    UnitType t=*registry.find(name);
    t.maxHp=32767;t.healTime=0;t.weapons.clear();t.weapon.damage=0;
    return t;
}

struct Field {
    World w;
    explicit Field(const tak::sim::TypeRegistry& registry) {
        w.setPlayerCount(2);
        w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
        w.buildNavClasses(registry);
    }
    void run(int ticks) { for (int i=0;i<ticks;++i) w.tick(1.f/30.f); }
    // An enemy flyer airborne and held still: it takes off on a move, then waits.
    int heldFlyer(const UnitType* type,float x,float z,int climb=45) {
        const int id=w.spawn(type,x,z,0,1);
        w.order(id,x+3000,z,false);
        run(climb);
        w.orderWait(id,600,false);
        run(2);
        return id;
    }
    bool targets(int id,int targetId) {
        const auto* u=w.unit(id);
        return u && std::any_of(u->orders.begin(),u->orders.end(),[&](const auto& o){return o.targetId==targetId;});
    }
    // A unit's hit points, 0 once it is gone.
    Fixed hp(int id) {
        const auto* u=w.unit(id);
        return u && u->alive() ? u->hp : Fixed();
    }
};
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    for (bool crusades:{false,true}) {
        const char* balance=crusades ? "Crusades" : "standard";
        tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
        const UnitType gryphon=target(registry,"zongryp");
        const UnitType dummy=target(registry,"tarzom");
        // The Titan, and every other mobile unit whose attack weapon is melee.
        std::vector<std::string> roster{"arasmith"};
        for (const auto& [name,type]:registry.types())
            if (name!="arasmith" && !type.canFly && type.canMove && !type.isStructure() && meleeSlot(type)>=0 &&
                type.weapon.damage>0)
                roster.push_back(name);
        int ordered=0,orderedHits=0,idle=0,idleHits=0,walked=0,wanderers=0;
        for (const auto& name:roster) {
            const UnitType* type=registry.find(name);
            const int slot=meleeSlot(*type);
            // Only melee can strike: a god's spells (slot 1, 2) may sweep at a flyer.
            const bool meleeOnly=std::all_of(type->weapons.begin(),type->weapons.end(),
                [](const auto& w){return w.melee || w.damage<=0;}) || type->weaponSwitching;
            // Animals, monsters and townsfolk roam on their own; for them only
            // the orders and the damage say anything.
            // A Standby_Wander type (`wanders`) is one whatever this sample
            // shows: its stroll's radius is random and can be a few px.
            bool roams=type->wanders;
            if (!roams) {
                Field f(registry);
                const int id=f.w.spawn(type,1000,1000,0,0);
                f.run(300);
                roams=std::hypot(f.w.unit(id)->x.toFloat()-1000,f.w.unit(id)->z.toFloat()-1000)>2.f;
            }
            wanderers+=roams;
            // An attack ordered (under the flyer, then 120 px off it) and an idle
            // unit 120 px off it. The flyer is airborne and held still.
            for (int setup=0;setup<3;++setup) {
                static const char* const setups[]={"ordered under","ordered at 120","idle at 120"};
                Field f(registry);
                const int flyer=f.heldFlyer(&gryphon,1000,1000);
                const bool aloft=f.w.unit(flyer)->flightGroundMode==2;
                const float ax=f.w.unit(flyer)->x.toFloat(),az=f.w.unit(flyer)->z.toFloat();
                const int id=f.w.spawn(type,setup ? ax-120 : ax,setup ? az : az+4,0,0);
                if (type->weaponSwitching) f.w.setWeapon(id,slot);
                f.run(1);
                if (setup<2) f.w.attack(id,flyer,false);
                const Fixed hp=f.hp(flyer);
                const float x0=f.w.unit(id)->x.toFloat(),z0=f.w.unit(id)->z.toFloat();
                bool kept=false;
                for (int tick=0;tick<300;++tick) {
                    f.run(1);
                    kept=kept || (tick>=1 && f.targets(id,flyer));
                }
                const auto& u=*f.w.unit(id);
                const int lost=(hp-f.hp(flyer)).floorInt();
                const float moved=std::hypot(u.x.toFloat()-x0,u.z.toFloat()-z0);
                const bool walks=!roams && moved>2.f;
                if (name=="arasmith" || kept || (meleeOnly && lost>0) || walks)
                    std::printf("%s balance=%s unit=%s slot=%d aloft=%d %s=%d lost=%d moved=%.1f\n",
                        setups[setup],balance,name.c_str(),slot,int(aloft),setup<2 ? "kept" : "acquired",
                        int(kept),lost,moved);
                check(aloft,std::string(balance)+" "+name+": the flyer is airborne");
                if (setup<2) { ordered+=kept;orderedHits+=meleeOnly && lost>0; }
                else { idle+=kept;idleHits+=meleeOnly && lost>0; }
                walked+=walks;
            }
        }
        std::printf("roster balance=%s melee units=%zu (%d roam) ordered kept=%d hit=%d idle acquired=%d hit=%d"
            " walked=%d\n",balance,roster.size(),wanderers,ordered,orderedHits,idle,idleHits,walked);
        check(roster.size()>40,std::string(balance)+": the melee roster is covered");
        check(ordered==0,std::string(balance)+": Attack_Melee ends at once on an airborne target");
        check(orderedHits==0,std::string(balance)+": no ordered melee swing reaches a flyer in the air");
        check(idle==0,std::string(balance)+": an idle melee unit does not acquire a flyer in the air");
        check(idleHits==0,std::string(balance)+": nor strike it");
        check(walked==0,std::string(balance)+": no melee unit walks after a flyer in the air");

        // A landed flyer is on the ground (state 1): the Titan acquires it,
        // walks to it and hammers it, ordered or idle. Once it takes off the
        // attack ends (Attack_Melee's head) and the Titan stops.
        for (bool told:{true,false}) {
            Field f(registry);
            const int flyer=f.w.spawn(&gryphon,1100,1000,0,1);
            f.run(2);
            const bool landed=f.w.unit(flyer)->flightGroundMode==1;
            const int id=f.w.spawn(registry.find("arasmith"),1000,1000,0,0);
            if (told) f.w.attack(id,flyer,false);
            const Fixed hp=f.hp(flyer);
            int tick=0;
            for (;tick<600 && f.hp(flyer)==hp;++tick) f.run(1);
            const int lost=(hp-f.hp(flyer)).floorInt();
            std::printf("landed balance=%s %s landed=%d first hit tick=%d lost=%d\n",
                balance,told ? "ordered" : "idle",int(landed),tick,lost);
            check(landed,std::string(balance)+": the fixture flyer is landed");
            check(lost>0,std::string(balance)+(told ? ": an ordered Titan hammers a landed flyer"
                                                     : ": an idle Titan acquires and hammers a landed flyer"));
            if (!told) continue;
            f.w.order(flyer,2500,1000,false);
            int aloftAt=-1,droppedAt=-1;
            for (int t=0;t<120 && droppedAt<0;++t) {
                f.run(1);
                const auto* air=f.w.unit(flyer);
                if (aloftAt<0 && air && air->flightGroundMode==2) aloftAt=t;
                if (!f.targets(id,flyer)) droppedAt=t;
            }
            std::printf("takeoff balance=%s aloft tick=%d attack dropped tick=%d\n",balance,aloftAt,droppedAt);
            check(aloftAt>=0 && droppedAt>=aloftAt && droppedAt<=aloftAt+1,
                  std::string(balance)+": the attack ends as the flyer takes off");
        }

        // The fire-at-will sweep (412620) gives a melee slot a landed flyer
        // too: a Titan walking a plain move into one swings at it.
        {
            Field f(registry);
            const int flyer=f.w.spawn(&gryphon,1100,1000,0,1);
            f.run(2);
            const int id=f.w.spawn(registry.find("arasmith"),900,1000,0,0);
            f.w.order(id,1400,1000,false);
            const Fixed hp=f.hp(flyer);
            int swept=0;
            for (int tick=0;tick<600;++tick) {
                f.run(1);
                swept+=f.w.unit(id)->slotTarget[0]==flyer;
            }
            const int lost=(hp-f.hp(flyer)).floorInt();
            std::printf("sweep balance=%s landed=%d swept ticks=%d lost=%d\n",
                balance,int(f.w.unit(flyer)->flightGroundMode==1),swept,lost);
            check(swept>0 && lost>0,std::string(balance)+": a passing Titan's sweep takes a landed flyer");
        }

        // Attack_Melee also ends on a carried target (+0x114 & 4, layer 0):
        // a Titan's attack on a unit that boards a Roc goes.
        {
            Field f(registry);
            const int victim=f.w.spawn(&dummy,1300,1000,0,1);
            const int roc=f.w.spawn(registry.find("zonroc"),1340,1000,0,1);
            const int id=f.w.spawn(registry.find("arasmith"),1000,1000,0,0);
            f.w.attack(id,victim,false);
            f.run(30);
            const bool chasing=f.targets(id,victim);
            const bool boarded=f.w.embarkInto(victim,roc);
            f.run(2);
            std::printf("carried balance=%s chasing=%d boarded=%d kept=%d\n",
                balance,int(chasing),int(boarded),int(f.targets(id,victim)));
            check(chasing && boarded && !f.targets(id,victim),
                  std::string(balance)+": the attack ends when its target boards a transport");
        }

        // noairweapon (WeaponType +0xc8 0x8000) is judged against the target's
        // layer too (530580's head, the picker, 4de530): a Knight (melee) and a
        // Grenadier (ballistic, every slot noairweapon) hit a LANDED flyer,
        // ordered or idle. World judged it by the flyer's type and never did.
        for (const char* name:{"arapal","aragren"}) {
            for (bool told:{true,false}) {
                Field f(registry);
                const int flyer=f.w.spawn(&gryphon,1150,1000,0,1);
                f.run(2);
                const int id=f.w.spawn(registry.find(name),1000,1000,0,0);
                if (told) f.w.attack(id,flyer,false);
                const Fixed hp=f.hp(flyer);
                f.run(600);
                const int lost=(hp-f.hp(flyer)).floorInt();
                std::printf("noair-landed balance=%s unit=%s %s lost=%d\n",balance,name,told ? "ordered" : "idle",lost);
                check(lost>0,std::string(balance)+" "+name+(told ? ": an ordered noairweapon attack hits a landed flyer"
                                                               : ": an idle noairweapon unit takes a landed flyer"));
            }
            // In the air it stays out of reach: no order is kept, nothing lands.
            Field f(registry);
            const int flyer=f.heldFlyer(&gryphon,1150,1000);
            const int id=f.w.spawn(registry.find(name),1000,1000,0,0);
            f.w.attack(id,flyer,false);
            const Fixed hp=f.hp(flyer);
            bool kept=false;
            for (int tick=0;tick<300;++tick) { f.run(1); kept=kept || (tick>=1 && f.targets(id,flyer)); }
            check(!kept && hp==f.hp(flyer),std::string(balance)+" "+name+": noairweapon leaves a flyer in the air alone");
        }

        // Attack_Chase (403bb0), a walker's ranged attack, ends on a carried
        // target too (403c9c: +0x114 & 4): a Grenadier's attack on a unit that
        // boards a Roc goes instead of standing aimed at the transport.
        {
            Field f(registry);
            const int victim=f.w.spawn(&dummy,1600,1000,0,1);
            const int roc=f.w.spawn(registry.find("zonroc"),1640,1000,0,1);
            const int id=f.w.spawn(registry.find("aragren"),1000,1000,0,0);
            f.w.attack(id,victim,false);
            f.run(10);
            const bool chasing=f.targets(id,victim);
            const bool boarded=f.w.embarkInto(victim,roc);
            f.run(2);
            std::printf("carried-ranged balance=%s chasing=%d boarded=%d kept=%d\n",
                balance,int(chasing),int(boarded),int(f.targets(id,victim)));
            check(chasing && boarded && !f.targets(id,victim),
                  std::string(balance)+": a ranged attack ends when its target boards a transport");
        }

        // A swing at a ground unit splashes its own origin's height (529dc0's
        // 3D box distance): a flyer held overhead at cruise height is not hit.
        {
            Field f(registry);
            const int flyer=f.heldFlyer(&gryphon,600,1000,150);
            const float ax=f.w.unit(flyer)->x.toFloat(),az=f.w.unit(flyer)->z.toFloat();
            const int victim=f.w.spawn(&dummy,ax,az,0,1);
            const int id=f.w.spawn(registry.find("arasmith"),ax-40,az,0,0);
            f.w.attack(id,victim,false);
            const Fixed flyerHp=f.hp(flyer),victimHp=f.hp(victim);
            f.run(300);
            const int flyerLost=(flyerHp-f.hp(flyer)).floorInt();
            const int victimLost=(victimHp-f.hp(victim)).floorInt();
            const int altitude=f.w.unit(flyer) && f.w.unit(victim) ?
                (f.w.unit(flyer)->flightY-f.w.unit(victim)->groundY).floorInt() : -1;
            std::printf("splash balance=%s altitude=%d victim lost=%d flyer lost=%d\n",
                balance,altitude,victimLost,flyerLost);
            check(victimLost>0 && flyerLost==0,
                  std::string(balance)+": the Hammer's splash misses a flyer over its target");
        }
    }
    std::printf("%s\n",failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
