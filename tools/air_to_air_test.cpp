// AirToAir (TotalA.exe 0x412d40) and VTOL_Evade (0x413bc0): a flyer
// attacking a unit whose type can fly dogfights it (retail_motion_test holds
// the handlers' fixed answers); this checks World runs them: which attacks get the mission,
// that the attacker keeps flying, that its weapons reach a flying target (the
// Phoenix's air-to-air laser, the Thunder's bombs) and that a break-off is
// flown and handed back to the attack.
//
//   air_to_air_test <retail-install-dir>
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/sim.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ta::sim;

namespace {

constexpr float kTick=1.f/30;

struct Fight {
    int ticks=0,shots=0,hits=0,directHits=0,ordered=0,flying=0,still=0,firstStill=-1,dogfight=0,hover=0,evades=0,handedBack=0;
    float damage=0,closest=1e9f;
    uint64_t hash=0;
};

// A flyer that cannot die, of the given type, unarmed unless `armed`.
UnitType dummy(const UnitType& type,bool armed=false) {
    UnitType t=type;
    t.maxHp=30000;t.healTime=0;
    if (!armed) { t.weapons.clear();t.weapon={}; }
    return t;
}

// `attacker` attacks a flying `target` for `ticks` ticks on open ground. The
// target holds a point (re-ordered 12 px either side each second, as
// attack_sweep's hovering target) or flies a circle of radius `circle` round
// the attacker's start, 120 px of arc per 45-tick leg. `mutual`: they attack
// each other and neither is steered.
Fight fight(const TypeRegistry& registry,const UnitType& attacker,const UnitType& target,float circle,
            int ticks,bool mutual=false) {
    Fight f;
    World w;
    w.setPlayerCount(2);
    w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    w.buildNavClasses(registry);
    w.player(0).mana=1e6;w.player(1).mana=1e6;
    const float cx=2000,cz=2000;
    const int them=w.spawn(&target,cx+(circle>0 ? circle : 300.f),cz,0.f,1);
    w.unit(them)->standbyAllowed=false;
    w.setStance(them,2);w.unit(them)->moveState=0;w.unit(them)->fireState=0;
    int leg=0;
    auto steer=[&] {
        if (circle>0) {
            const float a=float(leg++)*std::min(0.3926991f,120.f/circle);
            w.order(them,cx+circle*std::cos(a),cz+circle*std::sin(a),false);
        } else w.order(them,cx+300.f,cz+((leg++&1) ? 12.f : -12.f),false);
    };
    steer();
    for (int i=0;i<240;++i) { if (i%30==29) steer(); w.tick(kTick); }
    const int me=w.spawn(&attacker,cx,cz,1.5707963f,0);
    w.attack(me,them,false);
    if (mutual) w.attack(them,me,false);
    float hp=w.unit(them)->hp.toFloat();
    // VTOL_Evade on either attacker (`them` attacks only when `mutual`):
    // pushed, then handed back once AirToAir has taken off again and
    // installed its own moving controller.
    bool evading[2]={false,false},resuming[2]={false,false};
    auto watch=[&](const Unit& u,int target,int k) {
        if (u.orders.empty() || u.orders.front().targetId!=target || !u.orders.front().airToAir) return;
        const auto& a=*u.orders.front().airToAir;
        const bool now=a.evade.has_value();
        if (now && !evading[k]) ++f.evades;
        resuming[k]=(resuming[k] || (evading[k] && !now)) && !now;
        if (resuming[k] && a.intercept && !a.evadeController) { ++f.handedBack;resuming[k]=false; }
        evading[k]=now;
    };
    for (int tick=0;tick<ticks;++tick) {
        if (!mutual && tick%(circle>0 ? 45 : 30)==0) steer();
        const Unit* a=w.unit(me);
        const int32_t x=a->x.v,y=a->flightY.v,z=a->z.v;
        w.tick(kTick);
        a=w.unit(me);
        const Unit* t=w.unit(them);
        if (!a || !t || !a->alive() || !t->alive()) break;
        ++f.ticks;
        f.shots+=std::popcount(a->firedWeapons);
        for (const auto& h:w.hits()) if (h.fromPlayer==0 && h.victimId==them) {
            ++f.hits;
            f.directHits+=h.weapon && h.weapon->kind!=Weapon::Kind::Dropped;   // not a bomb
        }
        if (t->hp.toFloat()<hp) f.damage+=hp-t->hp.toFloat();
        hp=t->hp.toFloat();
        f.closest=std::min(f.closest,std::hypot((a->x-t->x).toFloat(),(a->z-t->z).toFloat()));
        watch(*a,them,0);
        if (mutual) watch(*t,me,1);
        if (a->orders.empty() || a->orders.front().targetId!=them) continue;
        const Order& o=a->orders.front();
        ++f.ordered;
        // Stages 0 and 1 sleep a tick each before the first controller.
        if (a->flightGroundMode==2 && tick>=2) {
            ++f.flying;
            const bool held=a->x.v==x && a->flightY.v==y && a->z.v==z;
            f.still+=held;
            if (held && f.firstStill<0) f.firstStill=tick;
        }
        f.dogfight+=o.airToAir.has_value();
        f.hover+=o.flightGoal && (o.flightGoal->flags&1);   // AirToGroundHover's ring controller
    }
    f.hash=w.stateHash();
    return f;
}

}  // namespace

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        if (!ok) ++failures;
    };
    {
        const char* balance="standard";
        TypeRegistry registry;setupRegistry(registry,vfs);
        // The flying target: an unarmed Peeper that cannot die.
        const UnitType roc=dummy(*registry.find("armpeep"));
        // 4de530: every armed flyer that may attack a flyer gets AirToAir,
        // hoverattack or not, and keeps flying; against a target that cannot
        // fly it does not.
        int roster=0,dogfights=0,neverHeld=0,noHover=0;
        for (const auto& [id,type]:registry.types()) {
            if (!type.canFly || type.weapons.empty()) continue;
            if (std::none_of(type.weapons.begin(),type.weapons.end(),[](const Weapon& w){return !w.noAir;})) continue;
            ++roster;
            const Fight f=fight(registry,type,roc,0,300);
            dogfights+=f.ordered>0 && f.dogfight==f.ordered;
            neverHeld+=f.flying>0 && f.still==0;
            noHover+=f.hover==0;
            if (f.dogfight!=f.ordered || f.still || f.hover || !f.ordered)
                std::printf("roster balance=%s unit=%s ordered=%d dogfight=%d flying=%d still=%d (first %d) hover=%d\n",
                            balance,id.c_str(),f.ordered,f.dogfight,f.flying,f.still,f.firstStill,f.hover);
        }
        std::printf("roster balance=%s flyers=%d airtoair=%d never-held=%d no-hover-ring=%d\n",balance,roster,
                    dogfights,neverHeld,noHover);
        check(roster>=10 && dogfights==roster,"every armed flyer attacking a flyer runs AirToAir");
        check(neverHeld==roster,"a dogfighting flyer never holds still in flight");
        check(noHover==roster,"a hoverattack flyer does not hover-ring a flying target");
        {
            World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            auto ground=*registry.find("armpw");ground.maxHp=30000;ground.healTime=0;ground.maxVel={};
            ground.weapons.clear();ground.weapon={};
            const int target=w.spawn(&ground,2300,2000,0,1);
            const int beak=w.spawn(registry.find("armthund"),2000,2000,0,0);
            const int gryphon=w.spawn(registry.find("armbrawl"),2000,2100,0,0);
            w.attack(beak,target,false);w.attack(gryphon,target,false);
            bool air=false;
            for (int tick=0;tick<120;++tick) {
                w.tick(kTick);
                for (int id:{beak,gryphon}) {
                    const Unit* u=w.unit(id);
                    air=air || (!u->orders.empty() && u->orders.front().airToAir.has_value());
                }
            }
            check(!air,"a flyer attacking a ground unit does not run AirToAir");
        }

        // The Phoenix (no hoverattack, airtoairdistance 100) could stop at
        // its longest range (1280, the dropped bombs') where its 600 px
        // air-to-air laser cannot reach. Dogfighting, it closes to its target
        // and the laser hits. (The Kingdoms original was the Aerial
        // Juggernaut's arrows under its dropped cannons.)
        const auto* juggernaut=registry.find("armpnix");
        const auto* beak=registry.find("armthund");
        // No shipped TA file sets airtoairdistance or hoverattackdistance, so
        // every flyer takes the default reach, 100.
        {
            bool all100=true;
            for (const auto& [id,type]:registry.types())
                if (type.canFly && type.airToAirDistance!=100) {
                    std::printf("airtoairdistance %s=%d\n",id.c_str(),int(type.airToAirDistance));all100=false;
                }
            check(all100,"airtoairdistance defaults: with no hover distance, 100");
        }
        for (float circle:{0.f,500.f}) {
            const Fight j=fight(registry,*juggernaut,roc,circle,1800);
            std::printf("engagement balance=%s unit=armpnix target=%s shots=%d hits=%d laser-hits=%d damage=%.0f closest=%.0f\n",
                        balance,circle>0?"circling":"holding",j.shots,j.hits,j.directHits,j.damage,j.closest);
            check(j.directHits>=3 && j.damage>0,"the Phoenix's laser reaches a flyer it dogfights");
            const Fight b=fight(registry,*beak,roc,circle,1800);
            std::printf("engagement balance=%s unit=armthund target=%s shots=%d hits=%d damage=%.0f closest=%.0f\n",
                        balance,circle>0?"circling":"holding",b.shots,b.hits,b.damage,b.closest);
        }

        // AirToGround (41bf80) makes every slot automatic and only the
        // selected one manual at its stages 1 and 2 (41c308, 41c31f; Kingdoms
        // addresses): a Phoenix attacking one ground unit drops its bombs
        // (slot 0) on it while its laser (slot 1) picks its own targets, here
        // also a second ground unit 250 px away. (A flyer whose selected
        // weapon is its anti-air one, the Kingdoms Juggernaut's stage 1 case,
        // has no TA counterpart.)
        {
            const UnitType jugg=dummy(*juggernaut,true);
            auto ground=*registry.find("armpw");ground.maxHp=30000;ground.healTime=0;ground.maxVel={};
            ground.weapons.clear();ground.weapon={};
            World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const float cx=2000,cz=2000;
            const int ordered=w.spawn(&ground,cx+300,cz,0.f,1),other=w.spawn(&ground,cx+300,cz+250,0.f,1);
            const int me=w.spawn(&jugg,cx,cz,1.5707963f,0);
            w.attack(me,ordered,false);
            int ticks=0,mixed=0,run=0,sweptOther=0,bombsAtOrdered=0,bombsAtOther=0,lasersAtOther=0;
            for (int tick=0;tick<1800;++tick) {
                w.tick(kTick);
                const Unit* a=w.unit(me);
                if (!a || a->orders.empty() || a->orders.front().targetId!=ordered) continue;
                ++ticks;
                run+=a->orders.front().mission.stage>=3;
                mixed+=a->slotAuto==6 && a->slotManual==World::kSlotsAttack;
                for (int s:{1,2}) sweptOther+=a->slotTarget[size_t(s)]==other;
                for (const auto& h:w.hits()) {
                    if (h.fromPlayer!=0 || !h.weapon) continue;
                    const bool bomb=h.weapon==&jugg.weapons[0];
                    const bool laser=h.weapon==&jugg.weapons[1];
                    if (h.victimId==ordered) bombsAtOrdered+=bomb;
                    if (h.victimId==other) { bombsAtOther+=bomb;lasersAtOther+=laser; }
                }
            }
            std::printf("airtoground balance=%s ticks=%d run=%d mixed=%d swept-other=%d bombs->ordered=%d"
                        " bombs->other=%d lasers->other=%d\n",balance,ticks,run,mixed,sweptOther,bombsAtOrdered,
                        bombsAtOther,lasersAtOther);
            check(ticks>0 && run>0 && mixed>=run,
                  "AirToGround leaves the selected slot manual and the others automatic (stages 1 and 2)");
            check(bombsAtOrdered>=2 && bombsAtOther==0,"the Phoenix's bombs attack the ordered ground unit");
            check(sweptOther>0 && lasersAtOther>0,"the Phoenix's laser picks its own ground targets meanwhile");
        }

        // The Thunder's bomb (dropped) leaves with no vertical speed and the
        // bomber's own motion (TotalA.exe 0x49dd60) and falls under gravity,
        // so it never reaches a flyer above it (a Seahawk at 220 against
        // cruisealt 200). Whether one cruising under it (a Brawler at 60, a
        // Rapier at 50) is struck depends on where the Thunder lets go, which
        // its air mission decides (the Kingdoms-ported run, which drops
        // continuously here: docs/ta-port-plan.md, Open), so that case only
        // asks that bombs are released.
        for (const char* name:{"armbrawl","corape","armsehak"}) {
            const UnitType holding=dummy(*registry.find(name));
            const Fight b=fight(registry,*beak,holding,0,1800);
            std::printf("engagement balance=%s unit=armthund target=%s(cruisealt %d) shots=%d hits=%d damage=%.0f"
                        " closest=%.0f\n",balance,name,holding.cruiseAlt,b.shots,b.hits,b.damage,b.closest);
            if (holding.cruiseAlt<beak->cruiseAlt)
                check(b.shots>=3,"the Thunder releases bombs at a holding flyer cruising below it");
            else check(b.shots>0 && b.hits==0,"the Thunder's bombs pass under a holding flyer at its own height");
        }

        // A Freedom Fighter and an Avenger attacking each other: each is soon
        // on the other's tail or in its sights, breaks off into VTOL_Evade,
        // and goes back to the attack.
        {
            const UnitType one=dummy(*registry.find("armfig"),true),other=dummy(*registry.find("corveng"),true);
            const Fight f=fight(registry,one,other,300,1800,true);
            const Fight again=fight(registry,one,other,300,1800,true);
            std::printf("evade balance=%s pushes=%d handed-back=%d hash=%016llx\n",balance,f.evades,f.handedBack,
                        static_cast<unsigned long long>(f.hash));
            check(f.evades>0 && f.handedBack>0,"a dogfighting flyer breaks off into VTOL_Evade and resumes the attack");
            check(f.hash==again.hash,"the dogfight is deterministic");
        }
    }
    return failures?1:0;
}
