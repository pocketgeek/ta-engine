#pragma once
// The CPUs this process may use, by kind, and pinning a thread to one of them: what
// SIMULATION THREADS (Options > GAME) describes and hands out (SimPool::handOut), and
// what takserver --sim-cpus pins to. Scheduling only: nothing here can change a
// simulation's results.

#include "sim/simpool.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tak::sim {

struct CpuTopology : SimPool::Cpus {
    // L: the logical CPU ids in the order threads take them: one of each performance
    // core, then one of each efficiency core, then the remaining hyperthread siblings
    // (performance cores' first). threads() entries.
    std::vector<int> order;
    bool canPin=true;   // false where the system lets no program pin a thread (macOS)
    // true: the system's own answer (Linux sysfs, the Windows API, macOS sysctl). false:
    // that could not be read, and this is the fallback of plain cores.
    bool known=false;
};

// The CPUs this process may use (its affinity mask on Linux and Windows), read anew on
// every call. Linux: sysfs (readLinuxCpus). Windows: GetLogicalProcessorInformationEx,
// processor group 0. macOS: sysctl hw.perflevel*, and canPin is false. Anything else,
// or any failure: std::thread::hardware_concurrency() plain cores, no hyperthreads.
// Debug builds: TAK_CPU_SYSFS=<root> reads a fixture tree in place of this machine
// (every cpuN it has), and TAK_CPU_NOPIN=1 reports a system that cannot pin.
CpuTopology detectCpus();

// The Linux reader, on the sysfs tree at `root` (/sys; a fixture in tests) for the CPUs
// in `allowed` (empty: every cpuN the tree has). A core is its thread_siblings_list
// (else its physical_package_id and core_id). The kind is Intel's hybrid lists
// (devices/cpu_core/cpus, devices/cpu_atom/cpus), else differing cpu_capacity values
// (the highest is a performance core), else there is no split; cores of one kind only
// are plain cores. A CPU whose topology cannot be read: every CPU is a core of its own.
CpuTopology readLinuxCpus(const std::filesystem::path& root,std::vector<int> allowed);

// A CCD (core complex die): the CPUs that share one L3 cache. PIN TO CCDS (Options >
// GAME) keeps the game on one and its local server on one (the same, if chosen).
struct Ccd {
    std::vector<int> cpus;                    // logical CPU ids, lowest first
    unsigned perf=0,eff=0,threads=0;          // its cores by kind (one kind: perf), and CPUs
    unsigned l1dKB=0,l1iKB=0,l2KB=0;          // each core's own caches; 0: not known
    unsigned l3KB=0;                          // the L3 its cores share; 0: not known
    unsigned maxMHz=0;                        // its fastest core's top clock; 0: not known
    constexpr unsigned cores() const { return perf+eff; }
};
// Every CPU the system has, grouped by the L3 each shares, by lowest CPU id: not only the
// CPUs this process may use, so the list does not shrink once the game is pinned to one.
// One entry for a CPU with a single L3 (or none reported); empty where the system says
// nothing (macOS). Linux: sysfs cache/index*; Windows: GetLogicalProcessorInformationEx,
// processor group 0. Debug builds: TAK_CPU_SYSFS reads a fixture tree, as detectCpus.
std::vector<Ccd> detectCcds();
// The Linux reader, on the sysfs tree at `root` (/sys; a fixture in tests).
std::vector<Ccd> readLinuxCcds(const std::filesystem::path& root);
// The threads a simulation gets on a CCD at AUTO: one for each performance core (each core,
// without the split), as SimPool::autoThreads.
inline unsigned ccdThreads(const Ccd& ccd) { return std::max(ccd.perf,1u); }
// Index of the CCD with the largest L3 when one is larger than all the others (an X3D's
// 3D V-Cache die), else -1.
int largestL3(const std::vector<Ccd>& ccds);

// The CPUs this process could use when it started (its affinity mask then), read on the
// first call: each program calls it early, before any change.
const std::vector<int>& launchCpus();
// Every thread of this process, those running and those started later, on `cpus` (only
// those it could use at launch); empty: back to launchCpus(). False, and nothing changed,
// where the system cannot pin (macOS) or no listed CPU may be used.
bool setProcessCpus(const std::vector<int>& cpus);

// "1,3,5-7" (a kernel cpulist, and takserver --sim-cpus): the ids in the order given.
// False for anything else, an empty text included.
bool parseCpuList(std::string_view text,std::vector<int>& out);
std::string cpuListText(const std::vector<int>& cpus);   // "1,3,5,6,7"
std::string cpuRangesText(const std::vector<int>& cpus); // "1,3,5-7" (sorted ids, runs as ranges)
// What a "sim threads:" log line says after its count: " pinned to cpus 4,6" for the
// CPUs in `cpus` (already text; several games' joined by " / "), then how many of the
// simulation's threads have none; nothing when `cpus` is empty.
std::string pinnedText(const std::string& cpus,unsigned unpinned);

// Whether this process may use CPU `cpu` (its affinity mask), so a thread can be pinned
// to it; never where the system cannot pin.
bool usableCpu(int cpu);
// Pin the calling thread to one CPU. False, and the thread is left as it was, for an
// id this process may not use (usableCpu), where the system cannot pin, and when the
// call fails (logged once): pinning never stops a game.
bool pinThisThread(int cpu);
// Give the calling thread every CPU the process may use again. A new thread starts on
// its creator's CPUs (Linux), so a thread started by a pinned one calls this unless it
// is pinned itself. Does nothing until some thread of the process has been pinned.
void unpinThisThread();
// A simulation thread's place: pinned to `cpu`, or (-1, or the pin failed) unpinned.
inline void placeThisThread(int cpu) { if (cpu<0 || !pinThisThread(cpu)) unpinThisThread(); }

} // namespace tak::sim
