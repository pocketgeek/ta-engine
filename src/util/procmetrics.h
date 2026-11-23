#pragma once
// Cross-platform process CPU + memory sampling for the benchmark stats screen. A Sample
// holds a process's CUMULATIVE CPU time (seconds) and resident memory (bytes); the caller
// diffs two samples over a wall-clock interval to get CPU%. Works for THIS process (pid 0)
// or another same-user process by pid (the local takserver via its child pid) -- Linux via
// /proc, macOS via libproc's proc_pid_rusage, Windows via GetProcessTimes/PSAPI.
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <string>

namespace tak::proc {

struct Sample {
    double cpuSeconds = 0;   // cumulative user+system CPU time
    size_t rssBytes = 0;     // resident set size (working set)
    bool ok = false;         // false = couldn't read this process
};

// Sample process `pid` (0 = this process). Returns ok=false if it can't be read.
Sample sample(long pid);

// Share of total machine CPU capacity, bounded like the system CPU display.
inline double processCpuPercent(const Sample& previous,const Sample& current,
                                double wallSeconds,int logicalCpus) {
    if(!previous.ok || !current.ok || !(wallSeconds>0) || !std::isfinite(wallSeconds) ||
       logicalCpus<=0 || !std::isfinite(current.cpuSeconds) || !std::isfinite(previous.cpuSeconds) ||
       current.cpuSeconds<previous.cpuSeconds)return -1;
    return std::clamp(100.0*(current.cpuSeconds-previous.cpuSeconds)/wallSeconds/logicalCpus,0.0,100.0);
}

// Cumulative OS CPU counters across all logical cores (units cancel in deltas).
struct SystemCpuSample {
    uint64_t total = 0, idle = 0;
    bool ok = false;
};
SystemCpuSample systemCpuSample();
inline double systemCpuPercent(const SystemCpuSample& previous, const SystemCpuSample& current) {
    if (!previous.ok || !current.ok || current.total <= previous.total || current.idle < previous.idle)
        return -1;
    const auto total = current.total - previous.total, idle = current.idle - previous.idle;
    if (idle > total) return -1;
    return 100.0 * double(total - idle) / double(total);
}

long selfPid();   // this process's pid
int numCpus();    // online logical CPUs (to report CPU% of one core vs. all cores)

// Perf busy and scheduled counters share nanosecond units. Parallel engines
// are evaluated separately; their percentages must never be added together.
inline double gpuBusyPercent(uint64_t oldBusy, uint64_t oldTime, uint64_t busy, uint64_t time) {
    if (busy < oldBusy || time <= oldTime) return -1;
    return std::clamp(100.0 * double(busy-oldBusy) / double(time-oldTime), 0.0, 100.0);
}

// Whole-device GPU statistics: NVIDIA NVML, Windows PDH GPU Engine counters,
// Linux AMD sysfs, macOS IOAccelerator, or Linux Intel i915 perf counters.
// Windows PDH and i915 report the busiest engine, not a sum of parallel engines.
// Unsupported or permission-blocked drivers report unavailable, never process-only
// utilization. Driver queries run off the render thread; no subprocess is launched.
struct GpuSample {
    double utilPct = -1;    // GPU utilization %, -1 if unknown
    size_t memUsed = 0;     // device VRAM used (all processes), bytes; 0 if unknown
    size_t memTotal = 0;    // total device VRAM, bytes; 0 if unknown
    std::string name;       // adapter name if known
    bool systemWide = false;// utilPct covers the WHOLE device, not this process
                            // (macOS: IOAccelerator "Device Utilization %" --
                            // Apple has no public per-process GPU stat)
    bool ok = false;        // false = no GPU stats source on this system
};
GpuSample gpuSample();

// Shared display contract for the HUD and benchmark results.
inline double systemGpuPercent(const GpuSample& sample) {
    if(!sample.ok || !sample.systemWide || !std::isfinite(sample.utilPct) || sample.utilPct<0)
        return -1;
    return std::clamp(sample.utilPct,0.0,100.0);
}

}  // namespace tak::proc
