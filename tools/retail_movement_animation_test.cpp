#include "client/retailmovementcallbacks.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
enum class Kind { Turn, MoveRate, Occupancy };
struct Event { Kind kind; int value; };
struct State { int turnSign=0; uint32_t moveRate=0,occupancy=0; };

const char* name(Kind kind) {
    switch(kind) {
    case Kind::Turn: return "TurnDirection";
    case Kind::MoveRate: return "MoveRate";
    case Kind::Occupancy: return "setSFXoccupy";
    }
    return "?";
}

std::vector<Event> update(State& state,int turnDegrees,uint32_t moveRate,uint32_t occupancy) {
    std::vector<Event> events;
    tak::updateRetailMovementAnimationCallbacks(true,turnDegrees,state.turnSign,
        moveRate,state.moveRate,occupancy,state.occupancy,
        [&](int value) { events.push_back({Kind::Turn,value}); },
        [&](uint32_t value) { events.push_back({Kind::MoveRate,int(value)}); },
        [&](uint32_t value) { events.push_back({Kind::Occupancy,int(value)}); });
    return events;
}

bool expect(const std::vector<Event>& actual,const std::vector<Event>& expected,const char* label) {
    if(actual.size()==expected.size()) {
        bool same=true;
        for(size_t i=0;i<actual.size();++i)
            same &= actual[i].kind==expected[i].kind && actual[i].value==expected[i].value;
        if(same)return true;
    }
    std::fprintf(stderr,"%s: callback sequence differed (got",label);
    for(const auto& e:actual)std::fprintf(stderr," %s(%d)",name(e.kind),e.value);
    std::fprintf(stderr,"; expected");
    for(const auto& e:expected)std::fprintf(stderr," %s(%d)",name(e.kind),e.value);
    std::fprintf(stderr,")\n");
    return false;
}

int trace() {
    State state;
    int turn;unsigned rate,occupancy;
    while(std::cin>>turn>>rate>>occupancy) {
        const auto events=update(state,turn,rate,occupancy);
        if(events.empty()) {
            std::cout<<"-\n";
            continue;
        }
        for(size_t i=0;i<events.size();++i) {
            if(i)std::cout<<' ';
            std::cout<<name(events[i].kind)<<'('<<events[i].value<<')';
        }
        std::cout<<'\n';
    }
    return 0;
}
}

int main(int argc,char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--trace")==0)return trace();
    State state;
    if(!expect(update(state,-135,1,5),
               {{Kind::Turn,-135},{Kind::MoveRate,1},{Kind::Occupancy,5}},
               "simultaneous movement changes"))return 1;
    if(!expect(update(state,-135,1,5),{},"unchanged tick"))return 1;
    if(!expect(update(state,-75,3,4),
               {{Kind::MoveRate,3},{Kind::Occupancy,4}},"same turn direction"))return 1;
    if(!expect(update(state,90,3,4),{{Kind::Turn,90}},"turn reversal"))return 1;
    if(!expect(update(state,0,3,4),{{Kind::Turn,0}},"turn stop"))return 1;
    std::puts("PASS: TurnDirection precedes MoveRate and occupancy, with native state-change suppression");
    return 0;
}
