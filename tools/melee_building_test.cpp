// Melee attackers on a building. Retail's Attack_Melee (404690) sends a melee
// attacker at a target with no mover (any building, a factory with canmove=1
// too) to 4d4e80's rectangle round its footprint, the controller RECLAIM,
// HelpBuild and RepairUnit use: each attacker walks to its own nearest origin
// on it, any origin where the footprints touch is arrival, corners included,
// and the melee in-range test (52b980) snaps both boxes to their footprint
// cells, so an attacker standing on the rectangle always bites. Through
// protocol 306 World sent every attacker at the building's centre: in Legion
// a lone or late attacker shared one goal cell (the second Spirit Wolf on a
// lodestone held behind the first for good, the owner's 0.9.12 replay), and a
// group stood on a ring sized from the building's longer side, out of contact
// on the short sides of every long building.
//
//   melee_building_test <data dir> <scenario dir>
//
// Runs the melee fixtures (tools/scenarios/melee-*.scn) in both pathfinding
// modes with the target's hit points topped up every tick, and counts each
// attacker's hits: the ticks the target lost hit points with that attacker as
// its lastHitBy. In the small fixtures every attacker must land at least
// kMinHits; twenty Swordsmen round a Keep must mostly get a place on it (on
// protocol 306: 11 of the 20 bit, 87 and 106 hits in Retail and Legion;
// now 16, about 150).
#include "legion_scn.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures=0;
void check(bool ok,const std::string& message) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",message.c_str());
    if(!ok)++failures;
}

// A bite every 20-30 ticks once in contact; three is a few seconds of biting.
constexpr int kMinHits=3;

struct Case {
    const char* file;
    std::vector<const char*> attackers;   // groups
    const char* target;                   // a one-body group
    int biters=0,total=0;                 // 0: every attacker lands kMinHits; else this many bite, this many hits
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
    const int target=b->groups.at(c.target).front();
    std::vector<int> attackers;
    for(const char* g:c.attackers) for(int id:b->groups.at(g)) attackers.push_back(id);
    std::vector<int> hits(attackers.size(),0),first(attackers.size(),-1);
    tak::scn::OrderFeed feed(s,*b);
    for(uint32_t t=0;t<s.ticks;++t) {
        feed.apply(t);
        auto* u=w.unit(target);
        if(!u || !u->alive()) { check(false,std::string(c.file)+" "+modeName+": the target died (topped up)"); return; }
        const auto full=tak::sim::Fixed::fromInt(u->type->maxHp);
        u->hp=full;u->lastHitBy=0;
        w.tick(1.f/30);
        u=w.unit(target);
        if(!u || u->hp>=full) continue;
        for(size_t i=0;i<attackers.size();++i)
            if(u->lastHitBy==attackers[i]) { ++hits[i];if(first[i]<0)first[i]=int(t); }
    }
    std::string line;
    int least=1<<30,biters=0,total=0;
    for(size_t i=0;i<attackers.size();++i) {
        line+=" "+std::to_string(attackers[i])+":"+std::to_string(hits[i])+"@"+std::to_string(first[i]);
        least=std::min(least,hits[i]);
        biters+=hits[i]>0;total+=hits[i];
    }
    if(!c.biters)
        check(least>=kMinHits,std::string(c.file)+" "+modeName+": every attacker bites at least "+
              std::to_string(kMinHits)+" times (id:hits@first)"+line);
    else
        check(biters>=c.biters && total>=c.total,std::string(c.file)+" "+modeName+": "+std::to_string(biters)+" of "+
              std::to_string(attackers.size())+" attackers bite (at least "+std::to_string(c.biters)+"), "+
              std::to_string(total)+" hits (at least "+std::to_string(c.total)+") (id:hits@first)"+line);
}
} // namespace

int main(int argc,char** argv) {
    if(argc!=3) { std::fprintf(stderr,"usage: melee_building_test <data dir> <scenario dir>\n"); return 2; }
    const std::vector<Case> cases{
        {"melee-lode-late.scn",{"W1","W2"},"L"},
        {"melee-lode-together.scn",{"W1","W2"},"L"},
        {"melee-castle-4.scn",{"W"},"C"},
        {"melee-keep-late.scn",{"W1","W2","W3","W4"},"K"},
        {"melee-castle-fight.scn",{"W"},"C"},
        {"melee-castle-20.scn",{"S"},"C",14,130},
    };
    try {
        for(const auto& c:cases)
            for(auto mode:{tak::sim::PathfindingMode::Retail,tak::sim::PathfindingMode::Legion})
                run(argv[2],argv[1],c,mode);
    } catch(const std::exception& e) {
        std::fprintf(stderr,"melee_building_test: %s\n",e.what());
        return 2;
    }
    std::printf("melee_building: %d failures\n",failures);
    return failures?1:0;
}
