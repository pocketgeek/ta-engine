#include "server/roomtick.h"
#include "server/rosterdump.h"
#include "ai/ai.h"
#include "sim/matchsetup.h"
#include "sim/simcpus.h"
#include "util/devenv.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
namespace ta::srv {
TickResult simulateRoomTick(sim::World& world,const sim::TypeRegistry& registry,
                            std::vector<ai::Controller>& controllers,TickInput input) {
    using namespace net;
    TickResult result;
    // Between ticks, on the thread that ticks this World: its pool is rebuilt at
    // the next pooled pass if the width changed.
    if(input.simThreads)world.setSimWorkers(int(input.simThreads)-1);
    if(input.simPin) {
        // This thread is the game's own worker (slot 0): placed anew when its CPU
        // changes, as games start and end. The pool's workers follow (setSimCpus).
        static thread_local int placed=-2;
        const int cpu=input.simCpus.empty()?-1:input.simCpus.front();
        if(cpu!=placed) {sim::placeThisThread(cpu);placed=cpu;}
        world.setSimCpus(std::move(input.simCpus));
    }
    const auto start=std::chrono::steady_clock::now();
    for(auto& controller:controllers)
        controller.tick(world,input.tick,[&](const Command& command) {input.commands.push_back(command);});
    static const bool report=ta::devEnv("TA_AIPHASE")!=nullptr;
    if(report) {
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        static const double threshold=ta::devEnv("TA_AIPHASE_MS")?std::atof(ta::devEnv("TA_AIPHASE_MS")):8.0;
        if(ms>threshold)std::fprintf(stderr,"AIPHASE tick=%u ai=%.1fms controllers=%zu room=%u\n",input.tick,ms,controllers.size(),input.roomId);
    }
    // Preserve the existing FIFO: immediate humans, AI, delayed humans. The
    // stable player sort retains each player's relative command order.
    input.commands.insert(input.commands.end(),input.scheduled.begin(),input.scheduled.end());
    std::stable_sort(input.commands.begin(),input.commands.end(),
        [](const Command& a,const Command& b) {return a.player<b.player;});
    Writer w;putBundle(w,input.tick,input.commands,input.events);
    if(w.b.size()+2*sizeof(std::vector<uint8_t>)>input.replayBudget) {
        result.resourceLimited=true;return result;
    }
    result.bundle=std::move(w.b);
    for(const auto& command:input.commands)sim::applyCommand(world,registry,command);
    for(const auto& event:input.events)sim::applyEvent(world,event);
    world.tick(1.0f/kServerHz);
#ifndef NDEBUG
    rosterDump(input.roomId,world,controllers);
#endif
    if(input.tick%uint32_t(kHashPeriod)==0) {
        const bool trail=input.replayTrail && (input.tick/uint32_t(kHashPeriod))%10==0;
        if(input.clientHashes || trail)result.hash=world.stateHash();
        if(trail)result.posDigest=world.posDigest();
    }
    result.missionOutcome=world.missionOutcome();
    const int players=std::min(kMaxSlots,world.numPlayers());
    bool undefeated=false;
    result.fielded=input.fielded;
    for(int i=0;i<players;++i) {
        const auto& player=world.player(i);
        result.defeated[size_t(i)]=player.defeated;
        undefeated|=!player.defeated;
        if(player.unitCount>0)result.fielded|=uint16_t(1u<<i);
    }
    // The standing (TickResult::decided). The teams that have fielded units are
    // counted as World::updateOutcome counts the surviving ones: any team id, once.
    if(world.hasScenarioOutcomes())result.decided=world.scenarioOutcome(-1)!=0;
    else {
        int teams=0;
        for(int i=0;i<players;++i) {
            if(!(result.fielded&(1u<<i)))continue;
            bool first=true;
            for(int j=0;j<i && first;++j)
                first=!(result.fielded&(1u<<j)) || world.player(j).team!=world.player(i).team;
            teams+=first;
        }
        result.decided=teams>=2 && (world.winningTeam()>=0 || !undefeated);
    }
    if(result.decided)result.winningTeam=int8_t(std::clamp(world.winningTeam(),-1,126));
    return result;
}
}
