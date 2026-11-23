#pragma once
#include "util/procmetrics.h"
#include <vector>

namespace tak::proc {
GpuSample windowsGpuSample();
namespace windowsgpu_detail {
struct Counter { std::wstring instance; double value; bool valid = true; };
struct Adapter { uint32_t high, low; std::string name; size_t dedicatedBytes; };
// GPU Engine instances are per-process. Sum processes on each physical engine,
// then take the busiest engine; never sum engines/GPUs running in parallel.
GpuSample aggregate(const std::vector<Counter>& engines,
                    const std::vector<Counter>& dedicatedMemory,
                    const std::vector<Adapter>& adapters);
}
}
