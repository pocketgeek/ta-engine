#include "util/nvml.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
using namespace tak::proc;
using namespace tak::proc::nvml_detail;
namespace {
int deviceError=0,utilError=0,memoryError=0;
unsigned int busy=35;
unsigned long long used=123ull<<20;
int device(unsigned int index,Device* d) { *d=index==0 ? reinterpret_cast<Device>(1) : nullptr;return deviceError; }
int utilization(Device,Utilization* u) { *u={busy,99};return utilError; }
int memory(Device,Memory* m) { *m={456ull<<20,(456ull<<20)-used,used};return memoryError; }
int name(Device,char* n,unsigned int size) { std::snprintf(n,size,"Test NVIDIA GPU");return 0; }
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
}
int main(int argc,char** argv) try {
    Api api{};api.device=device;api.utilization=utilization;api.memory=memory;api.name=name;
    auto g=sampleDevice(api);
    require(g.ok && g.systemWide && systemGpuPercent(g)==35,"whole-device GPU counter, not memory utilization");
    require(g.memUsed==123ull<<20 && g.memTotal==456ull<<20 && g.name=="Test NVIDIA GPU","memory byte units and adapter name");
    busy=0;require(systemGpuPercent(sampleDevice(api))==0,"true idle is zero");
    busy=100;require(systemGpuPercent(sampleDevice(api))==100,"fully busy is 100");
    utilError=3;g=sampleDevice(api);
    require(systemGpuPercent(g)==-1 && g.memUsed==123ull<<20,"unsupported utilization preserves memory and reports N/A");
    memoryError=3;require(!sampleDevice(api).ok,"all unavailable metrics do not become zero usage");
    utilError=0;busy=35;g=sampleDevice(api);
    require(systemGpuPercent(g)==35 && g.memUsed==0 && g.memTotal==0,"unsupported memory preserves utilization");
    memoryError=0;busy=std::numeric_limits<unsigned int>::max();used=std::numeric_limits<unsigned long long>::max();
    require(!sampleDevice(api).ok,"invalid driver counters rejected");
    deviceError=15;require(!sampleDevice(api).ok,"lost device is unavailable");
    require(!sampleDevice({}).ok,"missing API is optional");
    auto actual=nvidiaGpuSample();
    if(argc>1 && std::strcmp(argv[1],"--require-device")==0)
        require(actual.ok && systemGpuPercent(actual)>=0,"real NVIDIA driver reports utilization");
    if(actual.ok)std::printf("NVML: %s, %.0f%%, %zu/%zu bytes\n",actual.name.c_str(),systemGpuPercent(actual),actual.memUsed,actual.memTotal);
    else std::puts("NVML unavailable: optional driver library/counters handled");
    std::puts("PASS: direct NVML metrics and unavailable-counter handling");
    return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
