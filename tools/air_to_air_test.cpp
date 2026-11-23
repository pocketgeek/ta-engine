// AirToAir (41ca50) and VTOL_Evade (41e3d0): a flyer attacking a unit whose
// type can fly dogfights it. tools/re/check_air_to_air.py compares the
// handlers, their facing test and their moving controller with the original
// executable; this checks World runs them: which attacks get the mission,
// that the attacker keeps flying, that its weapons reach a flying target (the
// Aerial Juggernaut's arrows, the Iron Beak's Egg Bomb) and that a break-off
// is flown and handed back to the attack.
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

using namespace tak::sim;

namespace {

constexpr float kTick=1.f/30;

struct Fight {
    int ticks=0,shots=0,hits=0,ordered=0,flying=0,still=0,firstStill=-1,dogfight=0,hover=0,evades=0,handedBack=0;
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
    w.unit(me)->mana=attacker.maxMana;
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
        for (const auto& h:w.hits()) if (h.fromPlayer==0 && h.victimId==them) ++f.hits;
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
    auto vfs=tak::hpi::mountRetailRoot(argv[1]);
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        if (!ok) ++failures;
    };
    for (bool crusades:{false,true}) {
        const char* balance=crusades?"Crusades":"standard";
        TypeRegistry registry;setupRegistry(registry,vfs,crusades);
        const UnitType roc=dummy(*registry.find("zonroc"));
        // 4de530: every armed flyer that may attack a flyer gets AirToAir,
        // hoverattack or not, and keeps flying; against a target that cannot
        // fly it does not.
        int roster=0,dogfights=0,neverHeld=0,noHover=0;
        for (const auto& [id,type]:registry.types()) {
            if (!type.canFly || type.weapons.empty()) continue;
            if (std::none_of(type.weapons.begin(),type.weapons.end(),[](const Weapon& w){return !w.noAir;})) continue;
            ++roster;
            // (A Harpy charms the target and its order ends: count the ticks it lasts.)
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
        check(roster>=18 && dogfights==roster,"every armed flyer attacking a flyer runs AirToAir");
        check(neverHeld==roster,"a dogfighting flyer never holds still in flight");
        check(noHover==roster,"a hoverattack flyer does not hover-ring a flying target");
        {
            World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            auto ground=*registry.find("tarzom");ground.maxHp=30000;ground.healTime=0;ground.maxVel={};
            ground.weapons.clear();ground.weapon={};
            const int target=w.spawn(&ground,2300,2000,0,1);
            const int beak=w.spawn(registry.find("tarbeak"),2000,2000,0,0);
            const int gryphon=w.spawn(registry.find("zongryp"),2000,2100,0,0);
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

        // The Aerial Juggernaut (no hoverattack, airtoairdistance 100) used to
        // stop at its longest range (500, the dropped cannons') where its
        // 450 px anti-air arrows cannot reach, and drop cannonballs that miss.
        // Dogfighting, it closes to its target and the arrows hit.
        const auto* juggernaut=registry.find("creaeri");
        const auto* beak=registry.find("tarbeak");
        // 4c0501..4c054d: no shipped file sets airtoairdistance.
        check(juggernaut->airToAirDistance==100 && beak->airToAirDistance==212 &&
              registry.find("zondrag")->airToAirDistance==282 && registry.find("tarknigh")->airToAirDistance==230 &&
              registry.find("tarship")->airToAirDistance==70 &&
              registry.find("zonflies")->airToAirDistance==(crusades ? 156 : 212),
              "airtoairdistance defaults: the hover distance and altitude's hypotenuse, else 100");
        for (float circle:{0.f,500.f}) {
            const Fight j=fight(registry,*juggernaut,roc,circle,1800);
            std::printf("engagement balance=%s unit=creaeri target=%s shots=%d hits=%d damage=%.0f closest=%.0f\n",
                        balance,circle>0?"circling":"holding",j.shots,j.hits,j.damage,j.closest);
            check(j.hits>=3 && j.damage>0,"the Aerial Juggernaut's arrows reach a flyer it dogfights");
            const Fight b=fight(registry,*beak,roc,circle,1800);
            std::printf("engagement balance=%s unit=tarbeak target=%s shots=%d hits=%d damage=%.0f closest=%.0f\n",
                        balance,circle>0?"circling":"holding",b.shots,b.hits,b.damage,b.closest);
        }

        // Stage 1 (41d5ae) makes every slot automatic and only the selected
        // one manual: a Juggernaut dogfighting a Roc fights it with its arrows
        // (slot 0) while its cannons pick their own targets and bomb a ground
        // unit beneath the Roc, which used to be left alone while the cannons
        // dropped only at the Roc.
        {
            const UnitType jugg=dummy(*juggernaut,true),holding=dummy(*registry.find("zonroc"));
            auto ground=*registry.find("tarzom");ground.maxHp=30000;ground.healTime=0;ground.maxVel={};
            ground.weapons.clear();ground.weapon={};
            World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const float cx=2000,cz=2000;
            const int roc=w.spawn(&holding,cx+300,cz,0.f,1);
            w.unit(roc)->standbyAllowed=false;
            w.setStance(roc,2);w.unit(roc)->moveState=0;w.unit(roc)->fireState=0;
            const int zombie=w.spawn(&ground,cx+300,cz,0.f,1);
            int leg=0;
            auto steer=[&] { w.order(roc,cx+300.f,cz+((leg++&1) ? 12.f : -12.f),false); };
            steer();
            for (int i=0;i<240;++i) { if (i%30==29) steer(); w.tick(kTick); }
            const int me=w.spawn(&jugg,cx,cz,1.5707963f,0);
            w.attack(me,roc,false);
            int sweptGround=0,sweptRoc=0,ordered=0,mixed=0,arrowsAtRoc=0,cannonsAtGround=0,cannonsAtRoc=0,arrowsAtGround=0;
            // (The cannons kill a 30000 hp zombie inside a minute: count hp
            // lost while each is alive.)
            float rocHp=w.unit(roc)->hp.toFloat(),groundHp=w.unit(zombie)->hp.toFloat();
            float rocDamage=0,groundDamage=0;
            auto lost=[&](int id,float& hp,float& damage) {
                const Unit* t=w.unit(id);
                const float now=t && t->alive() ? t->hp.toFloat() : 0.f;
                if (now<hp) damage+=hp-now;
                hp=now;
            };
            for (int tick=0;tick<1800;++tick) {
                if (tick%30==0 && w.unit(roc) && w.unit(roc)->alive()) steer();
                w.tick(kTick);
                lost(roc,rocHp,rocDamage);lost(zombie,groundHp,groundDamage);
                const Unit* a=w.unit(me);
                if (!a || a->orders.empty() || a->orders.front().targetId!=roc) continue;
                ++ordered;
                mixed+=a->slotAuto==6 && a->slotManual==World::kSlotsAttack;
                for (int s:{1,2}) { sweptGround+=a->slotTarget[size_t(s)]==zombie;sweptRoc+=a->slotTarget[size_t(s)]==roc; }
                for (const auto& h:w.hits()) {
                    if (h.fromPlayer!=0 || !h.weapon) continue;
                    const bool arrow=h.weapon==&jugg.weapons[0];
                    const bool cannon=h.weapon==&jugg.weapons[1] || h.weapon==&jugg.weapons[2];
                    if (h.victimId==roc) { arrowsAtRoc+=arrow;cannonsAtRoc+=cannon; }
                    if (h.victimId==zombie) { arrowsAtGround+=arrow;cannonsAtGround+=cannon; }
                }
            }
            std::printf("cannons balance=%s ordered=%d mixed=%d arrows->roc=%d cannons->ground=%d cannons->roc=%d"
                        " arrows->ground=%d roc-damage=%.0f ground-damage=%.0f swept-ground=%d swept-roc=%d\n",balance,ordered,mixed,arrowsAtRoc,
                        cannonsAtGround,cannonsAtRoc,arrowsAtGround,rocDamage,groundDamage,sweptGround,sweptRoc);
            check(ordered>0 && mixed>=ordered-2 && rocDamage>0,
                  "AirToAir leaves the selected slot manual and the others automatic (stage 1)");
            check(arrowsAtRoc>=3 && arrowsAtGround==0,"the Juggernaut's arrows fight the flyer it is ordered onto");
            // (The sweep may hand a cannon the Roc too: a Dropped weapon's
            // in-range test is the plain 2D one, so retail's 412620 offers it.)
            check(sweptGround>0 && cannonsAtGround>=2 && groundDamage>0,
                  "the Juggernaut's cannons engage a ground unit beneath the flyer on their own");
            w.order(me,cx-400,cz,false);
            for (int tick=0;tick<5;++tick) w.tick(kTick);
            const Unit* a=w.unit(me);
            check(a && a->slotAuto==7 && a->slotManual==World::kSlotsAuto,"the slots are all automatic once the attack ends");
        }

        // AirToGround (41bf80) does the same at its stages 1 and 2 (41c308,
        // 41c31f): a Juggernaut attacking one ground unit shoots its arrows at
        // it while its cannons pick their own targets, here also a second
        // ground unit 250 px away; the cannons used to drop only at the
        // ordered one.
        {
            const UnitType jugg=dummy(*juggernaut,true);
            auto ground=*registry.find("tarzom");ground.maxHp=30000;ground.healTime=0;ground.maxVel={};
            ground.weapons.clear();ground.weapon={};
            World w;w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
            w.buildNavClasses(registry);
            const float cx=2000,cz=2000;
            const int ordered=w.spawn(&ground,cx+300,cz,0.f,1),other=w.spawn(&ground,cx+300,cz+250,0.f,1);
            const int me=w.spawn(&jugg,cx,cz,1.5707963f,0);
            w.attack(me,ordered,false);
            int ticks=0,mixed=0,run=0,sweptOther=0,arrowsAtOrdered=0,arrowsAtOther=0,cannonsAtOther=0;
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
                    const bool arrow=h.weapon==&jugg.weapons[0];
                    const bool cannon=h.weapon==&jugg.weapons[1] || h.weapon==&jugg.weapons[2];
                    if (h.victimId==ordered) arrowsAtOrdered+=arrow;
                    if (h.victimId==other) { arrowsAtOther+=arrow;cannonsAtOther+=cannon; }
                }
            }
            std::printf("airtoground balance=%s ticks=%d run=%d mixed=%d swept-other=%d arrows->ordered=%d"
                        " arrows->other=%d cannons->other=%d\n",balance,ticks,run,mixed,sweptOther,arrowsAtOrdered,
                        arrowsAtOther,cannonsAtOther);
            check(ticks>0 && run>0 && mixed>=run,
                  "AirToGround leaves the selected slot manual and the others automatic (stages 1 and 2)");
            check(arrowsAtOrdered>=2 && arrowsAtOther==0,"the Juggernaut's arrows attack the ordered ground unit");
            check(sweptOther>0 && cannonsAtOther>0,"the Juggernaut's cannons pick their own ground targets meanwhile");
        }

        // The Iron Beak's Egg Bomb (subtype Dropped, range 200) leaves with no
        // vertical speed and falls |dy| over its fall time toward the target's
        // SweetSpot (52c3b0), so it reaches a point below its muzzle only: a
        // flyer cruising under the Beak is hit, one at its height (a Roc's
        // SweetSpot is ~45 px above the egg) sees the egg pass under it.
        // Dogfighting 212 px out (airtoairdistance) the Beak still comes inside
        // the 200 px range: its pursuit point is 32 px nearer while it flies
        // above half speed, and a SweetSpot sits off its unit's centre.
        for (const char* name:{"aradrag","tarang","zonroc"}) {
            const UnitType holding=dummy(*registry.find(name));
            const Fight b=fight(registry,*beak,holding,0,1800);
            std::printf("engagement balance=%s unit=tarbeak target=%s(cruisealt %d) shots=%d hits=%d damage=%.0f"
                        " closest=%.0f\n",balance,name,holding.cruiseAlt,b.shots,b.hits,b.damage,b.closest);
            if (holding.cruiseAlt<beak->cruiseAlt)
                check(b.shots>=3 && 2*b.hits>=b.shots,"the Iron Beak's eggs hit a holding flyer cruising below it");
            else check(b.shots>0 && b.hits==0,"the Iron Beak's eggs pass under a holding flyer at its own height");
        }

        // A Gryphon and a Sea Dragon attacking each other: each is soon on the
        // other's tail or in its sights, breaks off into VTOL_Evade, and goes
        // back to the attack.
        {
            const UnitType one=dummy(*registry.find("zongryp"),true),other=dummy(*registry.find("verdrag"),true);
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
