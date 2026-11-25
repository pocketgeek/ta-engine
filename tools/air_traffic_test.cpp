// Air traffic: Kingdoms' flyers stepped out of a stack (a per-flyer counter,
// unit +0x12a, read by every flyer mission at its wake, pushing VTOL_StepOut
// at 5, or 8 for a flying builder at work). TotalA.exe has no step-out
// mission at all (no VTOL_StepOut* row or name in its mission tables,
// 0x4fc6e8 / 0x4fca18), so in TA no flyer ever steps out.
//
//   air_traffic_test <data dir> <scenario dir>
//
// Runs the air-stack-*.scn fixtures in both pathfinding modes and counts the
// ticks an airborne flyer of the case shares its footprint cells with another
// flyer (its overlap list is not empty): near the fight or the move's point (a
// target's hit points topped up to 30000 every tick), or while it flies the
// case's own leg -- a fight-move or a patrol, or a flying builder's job --
// (World has no step-out order any more: TA has none to push).
// air-guard-lost.scn checks
// VTOL_SeekGuard: fighters whose guarded Peewee dies guard another Peewee.
// (The fixtures came from Kingdoms -- Drakes, Goblins, a Keep -- and fly TA's
// Freedom Fighters, Peewees and an Aircraft Plant now.)
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

using ta::sim::Order;
using ta::sim::Unit;

// What a flyer of the case is doing: its front order.
const Order* leg(const Unit& u) {
    if(u.orders.empty())return nullptr;
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
};

void run(const std::string& dir,const char* data,const Case& c,ta::sim::PathfindingMode mode) {
    const char* modeName=mode==ta::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=ta::scn::load(dir+"/"+c.file);
    ta::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=ta::scn::build(s,options);
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
    ta::scn::OrderFeed feed(s,*b);
    int counted=0,stacked=0,site=0;
    long damage=0,mended=0;
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        float fx=c.x,fz=c.z;
        ta::sim::Fixed full;
        if(target && c.count!=Count::Repair && c.count!=Count::Help) {
            auto* u=w.unit(target);
            if(!u || !u->alive()) { check(false,std::string(c.file)+" "+modeName+": the target died (topped up)"); return; }
            // Far above any type's maximum: three volleys in one tick never
            // kill it.
            full=ta::sim::Fixed::fromInt(30000);
            u->hp=full;
            fx=u->x.toFloat();fz=u->z.toFloat();
        }
        if(c.count==Count::Repair && t==2) {
            // A fifth of the plant's hit points, and every builder sent to mend it.
            auto* keep=w.unit(target);
            keep->hp=ta::sim::Fixed::fromInt(keep->type->maxHp/5);
            for(int id:flyers) w.repair(id,target,false);
        }
        if(c.count==Count::Help && t==2) {
            // The first builder puts an Aircraft Plant's site down at the
            // point (its flying MobileBuild approaches it), the others help it.
            site=w.startBuild(flyers.front(),b->registry.find("armap"),c.x,c.z);
            for(size_t k=1;k<flyers.size();++k) w.assist(flyers[k],site,false);
        }
        const int mendedUnit=c.count==Count::Help ? site : target;
        const ta::sim::Fixed before=mendedUnit ? w.unit(mendedUnit)->hp : ta::sim::Fixed();
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
            stacked+=u->airOccupant.waiting;   // in a cell another flyer holds (0x47cc30)
        }
    }
    const char* what=c.count==Count::Near ? (target ? "near the fight" : "near the point")
                    : c.count==Count::Fight ? "on their fight-move legs" : c.count==Count::Patrol ? "on patrol"
                    : c.count==Count::Repair ? "repairing" : "building";
    const int permille=counted ? stacked*1000/counted : 0;
    check(counted>200 && permille<=c.maxStackedPermille,std::string(c.file)+" "+modeName+": "+std::to_string(stacked)+
          " of "+std::to_string(counted)+" airborne flyer-ticks "+what+" stacked ("+std::to_string(permille)+
          " per mille, at most "+std::to_string(c.maxStackedPermille)+")");
    // TA has no air-traffic check at all: TotalA.exe's air mission table
    // (0x4fca18) has no VTOL_StepOut* row, so none of its missions --
    // VTOL_Move 0x40fa20, VTOL_Patrol 0x410e70, AirToAir 0x412d40, the
    // builders' 0x413d80 / 0x414380 / 0x414e70 -- can push one (Kingdoms'
    // did: 417d4e, 419ee0, 41ce48, 41f0a6, 41f981, 421066); World has no
    // step-out order.
    if(target && c.count!=Count::Repair && c.count!=Count::Help)
        check(damage>0,std::string(c.file)+" "+modeName+": the fighters still hurt the target ("+
              std::to_string(damage)+" hit points)");
    if(c.count==Count::Repair || c.count==Count::Help)
        check(mended>0,std::string(c.file)+" "+modeName+": the builders still "+(c.count==Count::Help ? "build" : "mend")+
              " the Aircraft Plant ("+std::to_string(mended)+" hit points)");
}

// VTOL_SeekGuard: the fighters' guarded Peewee dies at tick 300; each
// fighter's guard, its last mission, goes on as VTOL_SeekGuard round where the
// Peewee stood and guards a Peewee of the ring round that point once one is within
// its sightdistance.
void guardLost(const std::string& dir,const char* data,ta::sim::PathfindingMode mode) {
    const char* modeName=mode==ta::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=ta::scn::load(dir+"/air-guard-lost.scn");
    ta::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=ta::scn::build(s,options);
    if(!b->skipped.empty()) { check(false,std::string("air-guard-lost.scn: ")+b->skipped); return; }
    auto& w=*b->world;
    const auto& drakes=b->groups.at("drakes");
    const int ward=b->groups.at("ward").front();
    const auto& ring=b->groups.at("ring");
    ta::scn::OrderFeed feed(s,*b);
    int guardingBefore=0,guardingRing=0,idleAfter=0;
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        if(t==300) if(auto* u=w.unit(ward)) u->hp=ta::sim::Fixed();   // the death sweep takes it
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
          " of 3 fighters guard the Peewee before it dies");
    check(guardingRing==3 && idleAfter==0,std::string("air-guard-lost.scn ")+modeName+": "+
          std::to_string(guardingRing)+" of 3 fighters guard a Peewee of the ring at the end ("+std::to_string(idleAfter)+
          " idle)");
}
// AirStrike (TotalA.exe 0x411f50): the fighters of air-stack-barracks.scn with
// the `dropped` key on their weapon. Their attack is the strike, not the
// hover: it makes its passes (stage 6, the turn after the release, comes round
// again and again), and, unlike Kingdoms', it has no air-traffic step-out.
void airStrike(const std::string& dir,const char* data,ta::sim::PathfindingMode mode) {
    const char* modeName=mode==ta::sim::PathfindingMode::Legion ? "legion" : "retail";
    const auto s=ta::scn::load(dir+"/air-stack-barracks.scn");
    ta::scn::BuildOptions options;
    options.mode=mode;
    options.serial=true;
    options.data=data;
    auto b=ta::scn::build(s,options);
    if(!b->skipped.empty()) { check(false,std::string("air-stack-barracks.scn: ")+b->skipped); return; }
    auto& w=*b->world;
    const auto& drakes=b->groups.at("drakes");
    const int keep=b->groups.at("keep").front();
    for(auto& weapon:const_cast<ta::sim::UnitType*>(w.unit(drakes.front())->type)->weapons) weapon.cursorAirstrike=true;
    ta::scn::OrderFeed feed(s,*b);
    std::map<int,uint8_t> stage;
    int turns=0;
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        if(auto* u=w.unit(keep)) u->hp=ta::sim::Fixed::fromInt(30000);
        w.tick(1.f/30);
        for(int id:drakes) {
            const auto* u=w.unit(id);
            if(!u || !u->alive() || u->orders.empty()) continue;
            const Order& front=u->orders.front();
            if(front.targetId==keep) {
                if(front.mission.stage==6 && stage[id]!=6) ++turns;
                stage[id]=front.mission.stage;
            }
        }
    }
    check(turns>=6,std::string("air-stack-barracks.scn ")+modeName+" with `dropped`: the fighters strike ("+
          std::to_string(turns)+" passes turned, at least 6)");
    // TotalA.exe's AirStrike (0x411f50) has no air-traffic check: its stage 4
    // (0x412394) only waits for the release ring. A stacked bomber strikes
    // from where it is.
}
} // namespace

int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"usage: air_traffic_test <data dir> <scenario dir>\n"); return 2; }
    // TA has no step-out (above), so a stack only breaks up as the missions
    // fly it apart: no bound but the per-mille itself, reported. With the
    // Kingdoms step-out these were bounded at 300, 300, 180, 210, 210, 150,
    // 150 (protocol 306, before any step-out: 992, 980, 282, 277); now (both
    // modes) 87, 83, 219, 227, 227, 136, 111. Flying builders on one job stack
    // too: VTOL_HelpBuild's hover point is builddistance from the site at the
    // builder's own bearing + 0xdb6e, every 150th tick (0x414569..0x41461a),
    // so helpers that came in together share it; the repair hovers over its
    // target (0x415048).
    const std::vector<Case> cases{
        {"air-stack-barracks.scn","drakes","keep",0,0,1000},
        {"air-stack-barracks-5.scn","drakes","keep",0,0,1000},
        {"air-stack-duel.scn","drakes","spy",0,0,1000},
        {"air-stack-move.scn","drakes",nullptr,1600,1280,1000},
        {"air-stack-fight.scn","drakes",nullptr,1600,1280,1000},
        {"air-stack-fight-cell.scn","d*",nullptr,0,0,1000,Count::Fight},
        {"air-stack-patrol.scn","drakes",nullptr,0,0,1000,Count::Patrol},
        {"air-stack-repair.scn","builders","keep",0,0,1000,Count::Repair},
        {"air-stack-help.scn","builders",nullptr,1280,1280,1000,Count::Help},
    };
    try {
        for(const auto& c:cases)
            for(auto mode:{ta::sim::PathfindingMode::Retail,ta::sim::PathfindingMode::Legion})
                run(argv[2],argv[1],c,mode);
        for(auto mode:{ta::sim::PathfindingMode::Retail,ta::sim::PathfindingMode::Legion})
            guardLost(argv[2],argv[1],mode);
        for(auto mode:{ta::sim::PathfindingMode::Retail,ta::sim::PathfindingMode::Legion})
            airStrike(argv[2],argv[1],mode);
    } catch(const std::exception& e) {
        std::fprintf(stderr,"air_traffic_test: %s\n",e.what());
        return 2;
    }
    std::printf("air_traffic: %d failures\n",failures);
    return failures?1:0;
}
