#include "util/windowsgpu.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace tak::proc;
using namespace tak::proc::windowsgpu_detail;
namespace {
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
Counter engine(unsigned pid,unsigned low,unsigned physical,unsigned id,double value) {
    return {L"pid_"+std::to_wstring(pid)+L"_luid_0x00000000_0x"+
        std::to_wstring(low)+L"_phys_"+std::to_wstring(physical)+L"_eng_"+
        std::to_wstring(id)+L"_engtype_3D",value};
}
}
int main() try {
    const std::vector<Adapter> adapters={{0,1,"AMD",1000},{0,2,"Intel",2000}};
    const std::vector<Counter> memory={{L"luid_0x00000000_0x00000001_phys_0",400},
                                     {L"luid_0x00000000_0x00000002_phys_0",500}};
    std::vector<Counter> engines={engine(1,1,0,0,35),engine(2,1,0,0,25),engine(1,1,0,1,50)};
    auto g=aggregate(engines,memory,adapters);
    require(g.ok && g.systemWide && systemGpuPercent(g)==60,"sum processes, not parallel engines");
    require(g.name=="AMD" && g.memUsed==400 && g.memTotal==1000,"same-adapter name and memory");
    engines.push_back(engine(3,2,0,0,90));g=aggregate(engines,memory,adapters);
    require(g.utilPct==90 && g.name=="Intel" && g.memUsed==500 && g.memTotal==2000,"busiest adapter selection");
    engines={engine(1,1,0,0,60),engine(2,1,1,0,60)};
    require(aggregate(engines,memory,adapters).utilPct==60,"physical engines remain separate");
    engines={engine(1,1,0,0,80),engine(2,1,0,0,80)};
    require(aggregate(engines,memory,adapters).utilPct==100,"percentage capped at 100");
    require(aggregate({engine(1,1,0,0,0)},memory,adapters).utilPct==0,"valid idle is zero");
    auto invalid=engine(1,1,0,0,90);invalid.valid=false;
    require(!aggregate({invalid,engine(1,1,0,0,-1),engine(1,1,0,0,
        std::numeric_limits<double>::quiet_NaN()),{L"bad instance",99}},memory,adapters).ok,"invalid counters unavailable");
    require(systemGpuPercent(aggregate({},memory,adapters))==-1,"missing engines report N/A");
    g=aggregate({engine(1,2,0,0,5)},{{L"luid_0x00000000_0x00000001_phys_0",999}},{});
    require(g.memUsed==0 && g.memTotal==0 && g.utilPct==5,"never mix memory from another adapter");
    auto actual=windowsGpuSample();
    if(actual.ok) std::printf("Windows GPU: %s, %.0f%%, %zu/%zu bytes\n",actual.name.c_str(),actual.utilPct,actual.memUsed,actual.memTotal);
    else std::puts("Windows GPU counters unavailable on this host");
    std::puts("PASS: Windows whole-system GPU aggregation");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
