// cpu_topology_test -- the REAL CPU detection (sim/simcpus.h) on the machine this runs on.
// Prints what it found, checks that the counts and the hand-out order agree, and pins the
// calling thread to the first CPU of the order: the call must not fail. Where the system
// pins no thread (macOS) it checks that the detection says so and that a pin is refused.
// The answer must be the system's own (CpuTopology::known) and count every CPU thread the
// process may use: a reader that fell back to plain cores fails here, not in a player's
// Options. The Linux reader's rules are pinned on fixture trees by tools/simpool_test.cpp;
// this is the one place the Windows and macOS code runs (CI's standalone checks, which
// also run it directly so that the log shows what a runner's CPUs were read as).
#include "sim/simcpus.h"

#include <algorithm>
#include <cstdio>

using namespace tak::sim;

static int failures=0;
static void check(bool ok,const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what);++failures; }
}

int main() {
    const CpuTopology cpus=detectCpus();
    std::printf("performance cores %u, efficiency cores %u, hyperthreads %u: %u threads; can pin: %s; read from the system: %s\n",
                cpus.perf,cpus.eff,cpus.smt,cpus.threads(),cpus.canPin ? "yes" : "no",cpus.known ? "yes" : "no (plain cores assumed)");
    std::printf("hand-out order: %s\n",cpuListText(cpus.order).c_str());
    std::printf("AUTO: %u threads; SimPool::usableCpus(): %u\n",SimPool::autoThreads(cpus),SimPool::usableCpus());
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    // The systems with a reader of their own: its fallback is a failure here.
    check(cpus.known,"the CPUs were read from the system, not assumed to be plain cores");
    check(cpus.threads()==SimPool::usableCpus(),"every CPU thread the process may use is counted, and no other");
#endif
    check(cpus.perf>=1 && cpus.threads()>=1,"at least one core");
    check(cpus.order.size()==cpus.threads(),"one entry of the order for every thread");
    std::vector<int> ids=cpus.order;
    std::sort(ids.begin(),ids.end());
    check(std::adjacent_find(ids.begin(),ids.end())==ids.end() && (ids.empty() || ids.front()>=0),
          "the order names each CPU once");
    check(SimPool::autoThreads(cpus)>=1 && SimPool::autoThreads(cpus)<=cpus.threads(),"AUTO is within the computer's threads");
    // Unpinned so far: unpinning is nothing to do, and must not fail either.
    unpinThisThread();
    if (cpus.canPin && !cpus.order.empty()) {
        check(pinThisThread(cpus.order.front()),"pinning this thread to the first CPU of the order");
        check(!pinThisThread(-1),"a CPU that does not exist is refused");
        unpinThisThread();
    } else {
        check(!cpus.canPin,"a system that cannot pin says so");
        check(!pinThisThread(cpus.order.empty() ? 0 : cpus.order.front()),"a pin is refused where the system cannot pin");
    }
    // CCDs (PIN TO CCDS): every CPU in exactly one, and the whole process pinned to the
    // first and back (Linux and Windows; macOS lists none and pins nothing).
    const std::vector<Ccd> ccds=detectCcds();
    for (size_t i=0;i<ccds.size();++i)
        std::printf("CCD %zu: cpus %s, %u performance + %u efficiency cores, %u threads; L1 %u+%u KB, L2 %u KB per core, L3 %u KB; up to %u MHz\n",
                    i+1,cpuRangesText(ccds[i].cpus).c_str(),ccds[i].perf,ccds[i].eff,ccds[i].threads,
                    ccds[i].l1dKB,ccds[i].l1iKB,ccds[i].l2KB,ccds[i].l3KB,ccds[i].maxMHz);
#if defined(__linux__) || defined(_WIN32)
    check(!ccds.empty(),"the CPUs fall into at least one CCD");
    unsigned grouped=0;
    for (const Ccd& c:ccds) grouped+=unsigned(c.cpus.size());
    check(grouped>=SimPool::usableCpus(),"every CPU is in a CCD");
    if (!ccds.empty() && cpus.canPin) {
        const std::vector<int> launch=launchCpus();
        std::vector<int> first;
        for (const int cpu:ccds.front().cpus) if (std::find(launch.begin(),launch.end(),cpu)!=launch.end()) first.push_back(cpu);
        check(first.empty() || setProcessCpus(first),"pinning the process to the first CCD");
        check(first.empty() || SimPool::usableCpus()==first.size(),"the process then uses that CCD's CPUs only");
        check(setProcessCpus({}) && SimPool::usableCpus()==launch.size(),"and back to every CPU it had");
    }
#else
    check(!setProcessCpus({0}),"a system that cannot pin refuses a process pin");
#endif
    std::printf("%s\n",failures ? "FAILED" : "PASS: this machine's CPUs are detected and a thread pins to the first of them");
    return failures ? 1 : 0;
}
