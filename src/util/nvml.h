#pragma once
#include "util/procmetrics.h"

namespace tak::proc {
// Optional, driver-provided NVML. No SDK or bundled vendor library is required.
GpuSample nvidiaGpuSample();

namespace nvml_detail {
// Minimal public NVML C ABI used by the optional loader. Layouts/signatures:
// https://docs.nvidia.com/deploy/nvml-api/api/group__nvmlDeviceQueries.html
// Memory is the unversioned nvmlMemory_t ABI, not nvmlMemory_v2_t.
using Device = void*;
struct Utilization { unsigned int gpu, memory; };
struct Memory { unsigned long long total, free, used; };
struct Api {
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*device)(unsigned int, Device*) = nullptr;
    int (*utilization)(Device, Utilization*) = nullptr;
    int (*memory)(Device, Memory*) = nullptr;
    int (*name)(Device, char*, unsigned int) = nullptr;
};
// Shared by the runtime loader and failure/unsupported-counter regression tests.
GpuSample sampleDevice(const Api& api);
}
}
