#include "util/procmetrics.h"
#include <cstdio>
int main() {
    using namespace tak::proc;
    // Eight cores supply 800 counter ticks: two busy cores mean 25%, not 200%.
    const SystemCpuSample before{1000,500,true};
    if(systemCpuPercent(before,{1800,1100,true})!=25 ||
       systemCpuPercent(before,{1800,500,true})!=100 ||
       systemCpuPercent(before,{1800,1300,true})!=0 ||
       systemCpuPercent({},before)!=-1 ||
       systemCpuPercent(before,before)!=-1 ||
       systemCpuPercent(before,{900,400,true})!=-1 ||
       systemCpuPercent(before,{1100,700,true})!=-1) return 1;
    if(gpuBusyPercent(100,1000,150,1100)!=50 ||
       gpuBusyPercent(100,1000,100,1100)!=0 ||
       gpuBusyPercent(100,1000,300,1100)!=100 ||
       gpuBusyPercent(100,1000,90,1100)!=-1 ||
       gpuBusyPercent(100,1000,110,1000)!=-1) return 1;
    const Sample processBefore{10,0,true};
    if(processCpuPercent(processBefore,{12,0,true},1,8)!=25 ||
       processCpuPercent(processBefore,{10,0,true},1,8)!=0 ||
       processCpuPercent(processBefore,{20,0,true},1,8)!=100 ||
       processCpuPercent(processBefore,{9,0,true},1,8)!=-1 ||
       processCpuPercent({},processBefore,1,8)!=-1 ||
       processCpuPercent(processBefore,{},1,8)!=-1 ||
       processCpuPercent(processBefore,{12,0,true},0,8)!=-1 ||
       processCpuPercent(processBefore,{12,0,true},1,0)!=-1)return 1;
    GpuSample gpu;
    if(systemGpuPercent(gpu)!=-1)return 1;
    gpu.ok=true;gpu.utilPct=35;
    if(systemGpuPercent(gpu)!=-1)return 1; // process-only source is not a system metric
    gpu.systemWide=true;
    if(systemGpuPercent(gpu)!=35)return 1;
    gpu.utilPct=150;
    if(systemGpuPercent(gpu)!=100)return 1;
    gpu.utilPct=0;
    if(systemGpuPercent(gpu)!=0)return 1;
    gpu.utilPct=-1;
    if(systemGpuPercent(gpu)!=-1)return 1;
    gpu.utilPct=std::nan("");
    if(systemGpuPercent(gpu)!=-1)return 1;
    std::puts("systemcpu: total utilization and unavailable/reset counters passed");
}
