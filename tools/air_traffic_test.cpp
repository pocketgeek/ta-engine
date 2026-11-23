// Retail's air traffic: flyers step out of a stack. After every occupancy
// rebuild a flyer's counter (unit +0x12a, retailAirTrafficStep) rises by the
// flyers its secondary occupancy list holds and falls by 2 when it holds
// none; each flyer mission, at its wake, pushes a step-out ahead of itself
// once the counter reaches 5 (8 for a flying builder at work: VTOL_StepOut
// 4214b0: the least crowded octant round the flyer's heading, D +- D/4 px
// out). Through protocol 306 World kept no counter and no step-out: Drakes
// hovering on one target or dogfighting one flyer piled onto a spot and
// stayed there (the owner's 0.9.12 replay: 42% of the hover ticks and 91% of
// the dogfight ticks stacked). Protocol 307's first cut left out the missions
// World did not fly as retail does: a flyer's fight-move legs and an armed
// flyer's patrol (no mission at all), the flying builders' jobs, and the
// guard's VTOL_SeekGuard (not ported); the owner's drakes on fight-move and
// guard still stacked.
//
//   air_traffic_test <data dir> <scenario dir>
//
// Runs the air-stack-*.scn fixtures in both pathfinding modes and counts the
// ticks an airborne flyer of the case shares its footprint cells with another
// flyer (its overlap list, the counter's input, is not empty): near the fight
// or the move's point (a target's hit points topped up to 30000 every tick),
// or while it flies the case's own leg -- a fight-move or a patrol, or a
// flying builder's repair -- step-outs that leg pushed included. A leg's case
// also counts the step-outs its mission pushed. air-guard-lost.scn checks
// VTOL_SeekGuard: Drakes whose guarded Goblin dies guard the other Goblin.
#include "ai/classicbrain.h"
#include "legion_scn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures=0;
void check(bool ok,const std::string& message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message.c_str());
    if(!ok)++failures;
}

using tak::sim::Order;
using tak::sim::Unit;

// What a flyer of the case is doing: its front order, or the one a step-out
// at the front was pushed by.
const Order* leg(const Unit& u) {
    if(u.orders.empty())return nullptr;
    if(u.orders.front().stepOut)return u.orders.size()>1?&u.orders[1]:nullptr;
    return &u.orders.front();
}
bool fightLeg(const Order& o) { return o.attackMove && !o.patrol && !o.targetId; }
bool patrolLeg(const Order& o) { return o.patrol && !o.targetId; }

enum class Count { Near, Fight, Patrol, Repair, Help };
struct Case {
    const char* file;
    const char* group;        // the flyers counted
    const char* target;       // the attacked group (topped up), or nullptr: the move's point
    float x,z;                // the move's point (px), without a target
    int maxStackedPermille;   // the most stacked counted flyer-ticks, per mille, the case may show
    Count count=Count::Near;
    int minStepOuts=0;        // the least step-outs the counted leg's mission must push
};

void run(const std::string& dir,const char* data,const Case& c,tak::sim::PathfindingMode mode) {
    const char* modeName=mode==tak::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=tak::scn::load(dir+"/"+c.file);
    tak::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=tak::scn::build(s,options);
    if(!b->skipped.empty()) { check(false,std::string(c.file)+": "+b->skipped); return; }
    auto& w=*b->world;
    // The counted groups: one by name, or a trailing * for a name prefix.
    const std::string group=c.group;
    const bool prefix=!group.empty() && group.back()=='*';
    std::vector<int> flyers;
    for(const auto& [name,ids]:b->groups)
        if(prefix ? name.compare(0,group.size()-1,group,0,group.size()-1)==0 : name==group)
            flyers.insert(flyers.end(),ids.begin(),ids.end());
    const int target=c.target ? b->groups.at(c.target).front() : 0;
    tak::scn::OrderFeed feed(s,*b);
    int counted=0,stacked=0,stepOuts=0,site=0;
    // The Retail AI's read of a builder stepping out (ClassicBrain's first
    // mission): VTOL_StepOutNoFormation (class 14, flags 0), not its job.
    int aiReads=0,aiStepOuts=0;
    long damage=0,mended=0;
    std::map<int,uint32_t> stepped;   // a flyer's last step-out (its issue tick)
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        float fx=c.x,fz=c.z;
        tak::sim::Fixed full;
        if(target && c.count!=Count::Repair && c.count!=Count::Help) {
            auto* u=w.unit(target);
            if(!u || !u->alive()) { check(false,std::string(c.file)+" "+modeName+": the target died (topped up)"); return; }
            // Far above any type's maximum: three Fire Breaths in one tick
            // never kill it.
            full=tak::sim::Fixed::fromInt(30000);
            u->hp=full;
            fx=u->x.toFloat();fz=u->z.toFloat();
        }
        if(c.count==Count::Repair && t==2) {
            // A fifth of the Keep's hit points, and every builder sent to mend it.
            auto* keep=w.unit(target);
            keep->hp=tak::sim::Fixed::fromInt(keep->type->maxHp/5);
            for(int id:flyers) w.repair(id,target,false);
        }
        if(c.count==Count::Help && t==2) {
            // The first builder puts a Keep's site down at the point (its
            // flying MobileBuild approaches it), the others help it.
            site=w.startBuild(flyers.front(),b->registry.find("arakeep"),c.x,c.z);
            for(size_t k=1;k<flyers.size();++k) w.assist(flyers[k],site,false);
        }
        const int mendedUnit=c.count==Count::Help ? site : target;
        const tak::sim::Fixed before=mendedUnit ? w.unit(mendedUnit)->hp : tak::sim::Fixed();
        w.tick(1.f/30);
        if(mendedUnit) if(const auto* u=w.unit(mendedUnit)) {
            if(c.count==Count::Repair || c.count==Count::Help) mended+=std::max(0,(u->hp-before).floorInt());
            else damage+=(full-u->hp).floorInt();
        }
        for(int id:flyers) {
            const auto* u=w.unit(id);
            if(!u || !u->alive() || u->flightGroundMode!=2) continue;
            const Order* o=leg(*u);
            bool counts=false;
            switch(c.count) {
            case Count::Near: counts=std::hypot(u->x.toFloat()-fx,u->z.toFloat()-fz)<=400.f; break;
            case Count::Fight: counts=o && fightLeg(*o); break;
            case Count::Patrol: counts=o && patrolLeg(*o); break;
            case Count::Repair: counts=u->repairId==target; break;
            case Count::Help: counts=site && u->buildSiteId==site; break;
            }
            if(!counts) continue;
            ++counted;
            stacked+=u->airOccupant.count!=0;
            // The leg's own step-outs: VTOL_StepOut ahead of a fight-move or
            // patrol leg, VTOL_StepOutNoFormation ahead of a builder's job.
            const bool builder=c.count==Count::Repair || c.count==Count::Help;
            if(!u->orders.empty() && u->orders.front().stepOut==(builder ? 3 : 1) &&
               stepped[id]!=u->orders.front().issuedTick+1) {
                stepped[id]=u->orders.front().issuedTick+1;
                ++stepOuts;
            }
            if(builder && !u->orders.empty() && u->orders.front().stepOut==3) {
                ++aiReads;
                aiStepOuts+=tak::ai::ClassicBrain::firstMissionOf(w,*u)==std::pair<uint8_t,uint32_t>{14,0};
            }
        }
    }
    const char* what=c.count==Count::Near ? (target ? "near the fight" : "near the point")
                    : c.count==Count::Fight ? "on their fight-move legs" : c.count==Count::Patrol ? "on patrol"
                    : c.count==Count::Repair ? "repairing" : "building";
    const int permille=counted ? stacked*1000/counted : 0;
    check(counted>200 && permille<=c.maxStackedPermille,std::string(c.file)+" "+modeName+": "+std::to_string(stacked)+
          " of "+std::to_string(counted)+" airborne flyer-ticks "+what+" stacked ("+std::to_string(permille)+
          " per mille, at most "+std::to_string(c.maxStackedPermille)+")");
    if(c.minStepOuts)
        check(stepOuts>=c.minStepOuts,std::string(c.file)+" "+modeName+": "+std::to_string(stepOuts)+
              " step-outs "+what+" (at least "+std::to_string(c.minStepOuts)+")");
    if(c.count==Count::Repair || c.count==Count::Help)
        check(aiReads>0 && aiStepOuts==aiReads,std::string(c.file)+" "+modeName+": the Retail AI reads the step-out "+
              "ahead of the job ("+std::to_string(aiStepOuts)+" of "+std::to_string(aiReads)+" flyer-ticks)");
    if(target && c.count!=Count::Repair && c.count!=Count::Help)
        check(damage>0,std::string(c.file)+" "+modeName+": the Drakes still hurt the target ("+
              std::to_string(damage)+" hit points)");
    if(c.count==Count::Repair || c.count==Count::Help)
        check(mended>0,std::string(c.file)+" "+modeName+": the builders still "+(c.count==Count::Help ? "build" : "mend")+
              " the Keep ("+std::to_string(mended)+" hit points)");
}

// VTOL_SeekGuard: the Drakes' guarded Goblin dies at tick 300; each Drake's
// guard, its last mission, goes on as VTOL_SeekGuard round where the Goblin
// stood and guards a Goblin of the ring round that point once one is within
// its sightdistance.
void guardLost(const std::string& dir,const char* data,tak::sim::PathfindingMode mode) {
    const char* modeName=mode==tak::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=tak::scn::load(dir+"/air-guard-lost.scn");
    tak::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=tak::scn::build(s,options);
    if(!b->skipped.empty()) { check(false,std::string("air-guard-lost.scn: ")+b->skipped); return; }
    auto& w=*b->world;
    const auto& drakes=b->groups.at("drakes");
    const int ward=b->groups.at("ward").front();
    const auto& ring=b->groups.at("ring");
    tak::scn::OrderFeed feed(s,*b);
    int guardingBefore=0,guardingRing=0,idleAfter=0;
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        if(t==300) if(auto* u=w.unit(ward)) u->hp=tak::sim::Fixed();   // the death sweep takes it
        w.tick(1.f/30);
        for(int id:drakes) {
            const auto* u=w.unit(id);
            if(!u || !u->alive()) continue;
            const Order* o=leg(*u);
            if(t==299) guardingBefore+=o && o->guard && o->targetId==ward;
            if(t==s.ticks-1) {
                guardingRing+=o && o->guard && std::find(ring.begin(),ring.end(),o->targetId)!=ring.end();
                idleAfter+=u->orders.empty();
            }
        }
    }
    check(guardingBefore==3,std::string("air-guard-lost.scn ")+modeName+": "+std::to_string(guardingBefore)+
          " of 3 Drakes guard the Goblin before it dies");
    check(guardingRing==3 && idleAfter==0,std::string("air-guard-lost.scn ")+modeName+": "+
          std::to_string(guardingRing)+" of 3 Drakes guard a Goblin of the ring at the end ("+std::to_string(idleAfter)+
          " idle)");
}
// AirStrike (41b5e0): the Drakes of air-stack-barracks.scn with the `dropped`
// key on their weapon (WeaponType +0xc8 bit 0x20; no shipped weapon has it).
// Their attack is the strike, not the hover: it makes its passes (stage 6, the
// turn after the release, comes round again and again), and its stage-4
// air-traffic check steps a Drake out of the stack the three start in.
void airStrike(const std::string& dir,const char* data,tak::sim::PathfindingMode mode) {
    const char* modeName=mode==tak::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=tak::scn::load(dir+"/air-stack-barracks.scn");
    tak::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=tak::scn::build(s,options);
    if(!b->skipped.empty()) { check(false,std::string("air-stack-barracks.scn: ")+b->skipped); return; }
    auto& w=*b->world;
    const auto& drakes=b->groups.at("drakes");
    const int keep=b->groups.at("keep").front();
    for(auto& weapon:const_cast<tak::sim::UnitType*>(w.unit(drakes.front())->type)->weapons) weapon.cursorAirstrike=true;
    tak::scn::OrderFeed feed(s,*b);
    std::map<int,uint8_t> stage;
    std::map<int,uint32_t> stepped;
    int turns=0,stepOuts=0;
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        if(auto* u=w.unit(keep)) u->hp=tak::sim::Fixed::fromInt(30000);
        w.tick(1.f/30);
        for(int id:drakes) {
            const auto* u=w.unit(id);
            if(!u || !u->alive() || u->orders.empty()) continue;
            const Order& front=u->orders.front();
            if(front.targetId==keep) {
                if(front.mission.stage==6 && stage[id]!=6) ++turns;
                stage[id]=front.mission.stage;
            }
            if(front.stepOut==1 && u->orders.size()>1 && u->orders[1].targetId==keep &&
               stepped[id]!=front.issuedTick+1) {
                stepped[id]=front.issuedTick+1;
                ++stepOuts;
            }
        }
    }
    check(turns>=6,std::string("air-stack-barracks.scn ")+modeName+" with `dropped`: the Drakes strike ("+
          std::to_string(turns)+" passes turned, at least 6)");
    check(stepOuts>=1,std::string("air-stack-barracks.scn ")+modeName+" with `dropped`: "+std::to_string(stepOuts)+
          " step-outs ahead of the strike (at least 1)");
}
} // namespace

int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"usage: air_traffic_test <data dir> <scenario dir>\n"); return 2; }
    // Per mille stacked on protocol 306 (both modes): 992, 980, 282, 277;
    // with the step-out: 44, 58, 86, 141.
    const std::vector<Case> cases{
        {"air-stack-barracks.scn","drakes","keep",0,0,300},
        {"air-stack-barracks-5.scn","drakes","keep",0,0,300},
        {"air-stack-duel.scn","drakes","spy",0,0,180},
        {"air-stack-move.scn","drakes",nullptr,1600,1280,210},
        {"air-stack-fight.scn","drakes",nullptr,1600,1280,210},
        {"air-stack-fight-cell.scn","d*",nullptr,0,0,150,Count::Fight,3},
        {"air-stack-patrol.scn","drakes",nullptr,0,0,150,Count::Patrol,3},
        {"air-stack-repair.scn","builders","keep",0,0,200,Count::Repair,2},
        {"air-stack-help.scn","builders",nullptr,1280,1280,200,Count::Help,2},
    };
    try {
        for(const auto& c:cases)
            for(auto mode:{tak::sim::PathfindingMode::Retail,tak::sim::PathfindingMode::Legion})
                run(argv[2],argv[1],c,mode);
        for(auto mode:{tak::sim::PathfindingMode::Retail,tak::sim::PathfindingMode::Legion})
            guardLost(argv[2],argv[1],mode);
        for(auto mode:{tak::sim::PathfindingMode::Retail,tak::sim::PathfindingMode::Legion})
            airStrike(argv[2],argv[1],mode);
    } catch(const std::exception& e) {
        std::fprintf(stderr,"air_traffic_test: %s\n",e.what());
        return 2;
    }
    std::printf("air_traffic: %d failures\n",failures);
    return failures?1:0;
}
