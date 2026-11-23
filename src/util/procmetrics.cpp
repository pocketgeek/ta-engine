#include "util/procmetrics.h"
#include "util/nvml.h"
#include "util/windowsgpu.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <libproc.h>
#  include <unistd.h>
#  include <sys/resource.h>
#  include <mach/mach_time.h>
#  include <mach/mach.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#else   // Linux / other /proc systems
#  include <cstdio>
#  include <cstring>
#  include <unistd.h>
#  include <linux/perf_event.h>
#  include <sys/syscall.h>
#endif

namespace tak::proc {

int numCpus() {
    unsigned n = std::thread::hardware_concurrency();
    return n ? int(n) : 1;
}

#if defined(_WIN32)

long selfPid() { return long(GetCurrentProcessId()); }

Sample sample(long pid) {
    Sample s;
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid))
                   : GetCurrentProcess();
    if (!h) return s;
    FILETIME ct, et, kt, ut;
    if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        auto v = [](const FILETIME& f) {
            return (uint64_t(f.dwHighDateTime) << 32) | uint64_t(f.dwLowDateTime);  // 100ns units
        };
        s.cpuSeconds = double(v(kt) + v(ut)) / 1e7;   // 100ns -> seconds
        s.ok = true;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) s.rssBytes = size_t(pmc.WorkingSetSize);
    if (pid) CloseHandle(h);
    return s;
}

#elif defined(__APPLE__)

long selfPid() { return long(getpid()); }

Sample sample(long pid) {
    Sample s;
    int p = int(pid ? pid : getpid());
    rusage_info_v2 ri{};
    // proc_pid_rusage works for a same-user process without task_for_pid privileges.
    if (proc_pid_rusage(p, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
        // ri_user_time / ri_system_time are in MACH ABSOLUTE TIME units, not ns
        // (Apple QA1398). Intel Macs have a 1/1 timebase, so /1e9 happened to
        // work there; Apple Silicon's is 125/3 (24MHz ticks), which made CPU%
        // read ~42x too low. Convert via mach_timebase_info (constant per boot).
        static const double tickToSec = [] {
            mach_timebase_info_data_t tb{};
            mach_timebase_info(&tb);
            return tb.denom ? double(tb.numer) / double(tb.denom) / 1e9 : 1e-9;
        }();
        s.cpuSeconds = double(ri.ri_user_time + ri.ri_system_time) * tickToSec;
        s.rssBytes = size_t(ri.ri_resident_size);
        s.ok = true;
    }
    return s;
}

#else   // Linux

long selfPid() { return long(getpid()); }

Sample sample(long pid) {
    Sample s;
    long p = pid ? pid : long(getpid());
    char path[64];

    // CPU: /proc/<p>/stat, fields 14 (utime) + 15 (stime) in clock ticks. comm (field 2)
    // can contain spaces/parens, so parse from AFTER the last ')'.
    std::snprintf(path, sizeof path, "/proc/%ld/stat", p);
    if (FILE* f = std::fopen(path, "r")) {
        char buf[4096];
        size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[n] = 0;
        const char* rp = std::strrchr(buf, ')');
        if (rp) {
            // After ')' the fields are: state(3) ppid(4) ... utime(14) stime(15).
            // Field 3 (state) is a single NON-numeric char (e.g. "R"), so skip that
            // token first; fields 4..15 are then numeric -- read utime + stime.
            const char* q = rp + 1;
            while (*q == ' ') ++q;
            while (*q && *q != ' ') ++q;   // skip the state token
            int field = 4;
            unsigned long long utime = 0, stime = 0;
            while (*q && field <= 15) {
                while (*q == ' ') ++q;
                char* end = nullptr;
                unsigned long long val = std::strtoull(q, &end, 10);
                if (end == q) break;
                if (field == 14) utime = val;
                else if (field == 15) stime = val;
                q = end;
                ++field;
            }
            long hz = sysconf(_SC_CLK_TCK);
            if (hz > 0) { s.cpuSeconds = double(utime + stime) / double(hz); s.ok = true; }
        }
    }

    // RSS: /proc/<p>/status "VmRSS:  N kB".
    std::snprintf(path, sizeof path, "/proc/%ld/status", p);
    if (FILE* f = std::fopen(path, "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) {
                unsigned long long kb = std::strtoull(line + 6, nullptr, 10);
                s.rssBytes = size_t(kb) * 1024;
                break;
            }
        }
        std::fclose(f);
    }
    return s;
}

#endif

SystemCpuSample systemCpuSample() {
    SystemCpuSample s;
#if defined(_WIN32)
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto ticks = [](const FILETIME& f) {
            return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        // Windows includes idle time in kernel time.
        s.total = ticks(kernel) + ticks(user);
        s.idle = ticks(idle);
        s.ok = true;
    }
#elif defined(__APPLE__)
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    const mach_port_t host = mach_host_self();
    if (host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count) == KERN_SUCCESS) {
        for (int i = 0; i < CPU_STATE_MAX; ++i) s.total += cpu.cpu_ticks[i];
        s.idle = cpu.cpu_ticks[CPU_STATE_IDLE];
        s.ok = true;
    }
    mach_port_deallocate(mach_task_self(), host);
#else
    if (FILE* f = std::fopen("/proc/stat", "r")) {
        char line[512];
        if (std::fgets(line, sizeof line, f)) {
            unsigned long long user=0,nice=0,system=0,idle=0,wait=0,irq=0,softirq=0,steal=0;
            if (std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &user,&nice,&system,&idle,&wait,&irq,&softirq,&steal) >= 4) {
                // Guest fields are already included in user/nice. I/O wait is
                // idle CPU time; counting it as busy exaggerates utilization.
                s.total = user + nice + system + idle + wait + irq + softirq + steal;
                s.idle = idle + wait;
                s.ok = true;
            }
        }
        std::fclose(f);
    }
#endif
    return s;
}

// ---- GPU stats (whole device) -----------------------------------------------

namespace {
#if !defined(_WIN32) && !defined(__APPLE__)
bool readLL(const char* path, long long& out) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    long long v = 0;
    int n = std::fscanf(f, "%lld", &v);
    std::fclose(f);
    if (n != 1) return false;
    out = v;
    return true;
}

// i915 exports device-wide engine busy counters in nanoseconds through perf.
// Sample all engines over the same interval; report the busiest engine rather
// than summing parallel engines into a misleading value greater than 100%.
bool intelSystemGpuSample(GpuSample& g) try {
    struct Counter {
        int fd;
        uint64_t busy=0, enabled=0, running=0;
    };
    std::vector<Counter> counters;
    struct Cleanup {
        std::vector<Counter>& counters;
        ~Cleanup() { for (const auto& c:counters) close(c.fd); }
    } cleanup{counters};
    std::error_code ec;
    const std::filesystem::path root("/sys/bus/event_source/devices");
    for (const auto& entry:std::filesystem::directory_iterator(root,ec)) {
        const auto name=entry.path().filename().string();
        if (name!="i915" && !name.starts_with("i915_")) continue;
        long long type=0,cpu=0;
        if (!readLL((entry.path()/"type").c_str(),type) ||
            !readLL((entry.path()/"cpumask").c_str(),cpu)) continue;
        std::error_code eventError;
        for (const auto& event:std::filesystem::directory_iterator(entry.path()/"events",eventError)) {
            if (!event.path().filename().string().ends_with("-busy")) continue;
            unsigned long long config=0;
            FILE* file=std::fopen(event.path().c_str(),"r");
            if (!file) continue;
            const bool parsed=std::fscanf(file,"config=%llx",&config)==1;
            std::fclose(file);
            if (!parsed) continue;
            perf_event_attr attr{};
            attr.size=sizeof(attr);attr.type=uint32_t(type);attr.config=config;
            attr.read_format=PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
            const int fd=int(syscall(__NR_perf_event_open,&attr,-1,int(cpu),-1,PERF_FLAG_FD_CLOEXEC));
            if (fd<0) continue;   // kernel policy may require CAP_PERFMON
            uint64_t values[3]{};
            if (read(fd,values,sizeof(values))!=sizeof(values)) { close(fd);continue; }
            counters.push_back({fd,values[0],values[1],values[2]});
        }
    }
    if (counters.empty()) return false;
    // gpuSample runs on the HUD worker; no sleeps or perf queries on the render thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double busiest=-1;
    for (const auto& c:counters) {
        uint64_t values[3]{};
        if (read(c.fd,values,sizeof(values))!=sizeof(values) || values[0]<c.busy ||
            values[1]<=c.enabled || values[2]<=c.running) continue;
        // Busy ns / scheduled ns accounts for any perf multiplexing. Both are
        // cumulative counters sampled at each boundary, not process wall time.
        busiest=std::max(busiest,gpuBusyPercent(c.busy,c.running,values[0],values[2]));
    }
    if (busiest<0) return false;
    g.utilPct=busiest;g.systemWide=true;g.ok=true;g.name="Intel GPU (busiest engine)";
    return true;
} catch (const std::filesystem::filesystem_error&) {
    return false;   // hot-unplug or inaccessible sysfs: unavailable, never fatal
}

#endif
}  // namespace

GpuSample gpuSample() {
    GpuSample g;
#if defined(__APPLE__)
    // No public PER-PROCESS GPU stat exists on Apple Silicon (Metal has no
    // utilization query; IOReport is private). The accelerator driver does
    // publish device-wide utilization in its IORegistry PerformanceStatistics
    // dictionary ("Device Utilization %" -- what iStat-style tools read), so
    // report that, flagged systemWide for the overlay label. Undocumented but
    // long-stable; if absent, ok stays false and the overlay shows N/A.
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(/*main port*/ 0,
                                     IOServiceMatching("IOAccelerator"),
                                     &iter) == KERN_SUCCESS) {
        io_object_t obj;
        while (!g.ok && (obj = IOIteratorNext(iter))) {
            CFMutableDictionaryRef props = nullptr;
            if (IORegistryEntryCreateCFProperties(obj, &props, kCFAllocatorDefault, 0) ==
                    KERN_SUCCESS && props) {
                auto stats = (CFDictionaryRef)CFDictionaryGetValue(
                    props, CFSTR("PerformanceStatistics"));
                if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                    auto num = (CFNumberRef)CFDictionaryGetValue(
                        stats, CFSTR("Device Utilization %"));
                    int v = -1;
                    if (num && CFGetTypeID(num) == CFNumberGetTypeID() &&
                        CFNumberGetValue(num, kCFNumberIntType, &v) && v >= 0) {
                        g.utilPct = std::clamp(double(v), 0.0, 100.0);
                        g.systemWide = true;
                        g.ok = true;
                    }
                }
                CFRelease(props);
            }
            IOObjectRelease(obj);
        }
        IOObjectRelease(iter);
    }
    return g;
#else
    if (auto nvidia = nvidiaGpuSample(); nvidia.ok) return nvidia;
#if defined(_WIN32)
    return windowsGpuSample();
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    // AMD (Linux amdgpu): sysfs. gpu_busy_percent is 0..100; VRAM figures are bytes.
    for (int c = 0; c < 4; ++c) {
        char pbusy[96], pused[96], ptot[96];
        std::snprintf(pbusy, sizeof pbusy, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        std::snprintf(pused, sizeof pused, "/sys/class/drm/card%d/device/mem_info_vram_used", c);
        std::snprintf(ptot, sizeof ptot, "/sys/class/drm/card%d/device/mem_info_vram_total", c);
        long long busy = 0, used = 0, total = 0;
        if (readLL(pbusy, busy) && busy >= 0) {
            g.utilPct = std::clamp(double(busy), 0.0, 100.0);
            g.systemWide = true;
            if (readLL(pused, used)) g.memUsed = size_t(used);
            if (readLL(ptot, total)) g.memTotal = size_t(total);
            g.ok = true;
            return g;
        }
    }
    if (intelSystemGpuSample(g)) return g;
    // Do not substitute per-process DRM fdinfo for whole-device utilization.
    // Drivers without a system-wide counter leave utilization unavailable.
#endif
    return g;
#endif   // !__APPLE__
}

}  // namespace tak::proc
