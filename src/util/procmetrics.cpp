#include "util/procmetrics.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#else   // Linux / other /proc systems
#  include <cstdio>
#  include <cstring>
#  include <dirent.h>
#  include <unistd.h>
#endif

namespace ta::proc {

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

// ---- GPU stats (whole device) -----------------------------------------------

namespace {
std::string runCmd(const char* cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(cmd, "r");
#else
    FILE* p = popen(cmd, "r");
#endif
    if (!p) return out;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    return out;
}
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

// Intel (i915/xe) GPU via DRM client fdinfo (/proc/self/fdinfo/*). Engine busy time
// is a cumulative ns counter per DRM client, so we diff it against wall time between
// calls to get util%. This is THIS process's GPU time (not whole-device) -- the best
// a non-root reader gets on Intel; memUsed is the resident GPU memory (memTotal is
// unknown, an iGPU shares system RAM). Fails closed (returns false) if not Intel.
bool intelGpuSample(GpuSample& g) {
    DIR* d = opendir("/proc/self/fdinfo");
    if (!d) return false;
    std::set<long> seen;            // dedupe: many fds map to one DRM client
    unsigned long long engineNs = 0;
    size_t residentBytes = 0;
    bool anyIntel = false;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string path = std::string("/proc/self/fdinfo/") + de->d_name;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        char line[256];
        bool isIntel = false; long client = -1;
        unsigned long long eNs = 0; size_t resB = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "drm-driver:", 11) == 0)
                isIntel = std::strstr(line, "i915") || std::strstr(line, "xe");
            else if (std::strncmp(line, "drm-client-id:", 14) == 0)
                client = std::atol(line + 14);
            else if (std::strncmp(line, "drm-engine-", 11) == 0) {
                char* c = std::strchr(line, ':');   // "<N> ns" only (skip capacity lines)
                if (c && std::strstr(c, " ns")) eNs += std::strtoull(c + 1, nullptr, 10);
            } else if (std::strncmp(line, "drm-resident-", 13) == 0) {
                char* c = std::strchr(line, ':');
                if (c) {
                    unsigned long long v = std::strtoull(c + 1, nullptr, 10);
                    if (std::strstr(c, "MiB")) resB += size_t(v) << 20;
                    else if (std::strstr(c, "KiB")) resB += size_t(v) << 10;
                    else resB += size_t(v);
                }
            }
        }
        std::fclose(f);
        if (isIntel) {
            anyIntel = true;
            if (client < 0 || seen.insert(client).second) { engineNs += eNs; residentBytes += resB; }
        }
    }
    closedir(d);
    if (!anyIntel) return false;

    unsigned long long wallNs = (unsigned long long)
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    static unsigned long long prevEng = 0, prevWall = 0;
    if (prevWall != 0 && wallNs > prevWall) {
        double dEng = engineNs >= prevEng ? double(engineNs - prevEng) : 0.0;
        double u = dEng / double(wallNs - prevWall) * 100.0;
        g.utilPct = u < 0 ? 0.0 : (u > 100.0 ? 100.0 : u);
    } else {
        g.utilPct = 0.0;   // first sample: no delta yet
    }
    prevEng = engineNs; prevWall = wallNs;
    g.memUsed = residentBytes;
    g.memTotal = 0;        // integrated GPU shares system RAM -- no fixed VRAM total
    g.name = "Intel GPU";
    g.ok = true;
    return true;
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
                        g.utilPct = double(v);
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
    // NVIDIA (any OS with the driver in PATH): one nvidia-smi CSV line.
#if defined(_WIN32)
    const char* nv = "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,name "
                     "--format=csv,noheader,nounits 2>nul";
#else
    const char* nv = "nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,name "
                     "--format=csv,noheader,nounits 2>/dev/null";
#endif
    std::string o = runCmd(nv);
    if (!o.empty()) {
        std::string line = o.substr(0, o.find('\n'));   // "util, usedMiB, totalMiB, Name"
        size_t p1 = line.find(','), p2 = p1 == std::string::npos ? p1 : line.find(',', p1 + 1),
               p3 = p2 == std::string::npos ? p2 : line.find(',', p2 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos) {
            g.utilPct = std::atof(line.substr(0, p1).c_str());
            g.memUsed = size_t(std::atoll(line.substr(p1 + 1, p2 - p1 - 1).c_str())) << 20;
            if (p3 != std::string::npos) {
                g.memTotal = size_t(std::atoll(line.substr(p2 + 1, p3 - p2 - 1).c_str())) << 20;
                g.name = line.substr(p3 + 1);
            } else {
                g.memTotal = size_t(std::atoll(line.substr(p2 + 1).c_str())) << 20;
            }
            while (!g.name.empty() && g.name.front() == ' ') g.name.erase(g.name.begin());
            while (!g.name.empty() && (g.name.back() == ' ' || g.name.back() == '\r' || g.name.back() == '\n'))
                g.name.pop_back();
            g.ok = true;
            return g;
        }
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    // AMD (Linux amdgpu): sysfs. gpu_busy_percent is 0..100; VRAM figures are bytes.
    for (int c = 0; c < 4; ++c) {
        char pbusy[96], pused[96], ptot[96];
        std::snprintf(pbusy, sizeof pbusy, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        std::snprintf(pused, sizeof pused, "/sys/class/drm/card%d/device/mem_info_vram_used", c);
        std::snprintf(ptot, sizeof ptot, "/sys/class/drm/card%d/device/mem_info_vram_total", c);
        long long busy = 0, used = 0, total = 0;
        if (readLL(pbusy, busy)) {
            g.utilPct = double(busy);
            if (readLL(pused, used)) g.memUsed = size_t(used);
            if (readLL(ptot, total)) g.memTotal = size_t(total);
            g.ok = true;
            return g;
        }
    }
    // Intel (i915/xe): DRM fdinfo. Only this process's GPU time, but better than N/A.
    if (intelGpuSample(g)) return g;
#endif
    return g;
#endif   // !__APPLE__
}

}  // namespace ta::proc
