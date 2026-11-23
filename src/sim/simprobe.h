#pragma once
// Debug-only sim instruments for the engine-wide perf work (WE E0.1). Nothing here
// exists in a release build (NDEBUG), nothing is hashed, and nothing changes what the
// sim computes: every counter only observes. Two switches, both read once:
//
//   TAK_SIMSTATS=1  one SIMSTATS line per tick (stderr) with work counters:
//                   vm_ticks / vm_skippable / vm_empty / vm_debt_flushes (the A1 COB
//                   sleep skip: VM ticks due, ticks skipped, skipped with no thread able
//                   to wake by itself, and skipped-tick debts paid by a touch from
//                   outside the VM's own tick; World::UnitScript),
//                   near_scans / near_cells / near_cells_masked / near_blocks_skipped /
//                   near_cells_block_skippable / near_cells_outside_disk /
//                   los_calls (forEachNear and combatLineOfSight, for A2),
//                   near_cells_tested (cells forEachNear actually tests once A2b's block
//                   skip has run; near_cells stays the plain square scan's count),
//                   near_cells_tested_outside_disk (disk scans' cells A2b leaves to test
//                   whose box is wholly outside the search radius + 2 px: what A2c could
//                   still remove; the disk scan was findTarget's, removed with the
//                   stride scan, so these two stay 0 until a disk caller returns),
//                   mission_scans (retailMissionScan calls: 4d8370),
//                   body_rects / body_rects_heap (searchBodyRect queries, and those too
//                   large for BodyCells' inline buffer; A5),
//                   refresh_rects / refresh_grade_evals / refresh_raw_grades (A3: search-plane
//                   refreshes, the plane cells they rewrite, and rawSearchGrade calls -- since E2.2
//                   only the per-cell reference makes those, so 0 in a normal run),
//                   refresh_cells_rated (cells the E2.2 box-minimum refresh grades once each),
//                   compact_moved, passes (the census of full sweeps over units_ in
//                   World::tick and the sim.cpp helpers it calls; Legion's own sweeps in
//                   legion.cpp are not counted), and minflt / minflt_sim / heap_grow (D6: minor page faults
//                   of the process and of the sim thread alone, and the change in malloc'd
//                   bytes, inside the tick; Linux/glibc only, 0 elsewhere).
//   TAK_PMU=1       one SIMPMU line per tick (stderr, needs TAK_PHASE): user-mode cycles,
//                   instructions, cycles stalled on L1-miss loads and LLC misses, read with
//                   rdpmc at the same phase boundaries TAK_PHASE times (Linux only). Pin the
//                   process to ONE core type: the events differ between P- and E-cores and
//                   are opened for the core type the sim thread first runs on.
//
// The counters slow the run down (TAK_SIMSTATS walks VM threads and scans the near grid a
// second time; TAK_PMU reads four counters at every per-unit phase boundary), so a timing
// run sets neither: counters first, then milliseconds, from separate runs.

namespace tak::sim::probe {
// Phases for the PMU split; the same buckets as the SIMPHASE line plus the prologue and the
// compaction (which runs after the tick total is taken, so it is not inside "tick").
// Declared in every build: the TAK_PHASE timers name their bucket with it.
enum Phase : int { kTotal, kCombat, kSep, kExplore, kVis, kBurn, kGrid, kScripts, kMovement, kNav, kPrologue, kCompact, kPhases };
}  // namespace tak::sim::probe

#ifndef NDEBUG
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <string>
#define TAK_PROBE_HAVE_PMU 1
#endif
#if defined(__linux__)
#include <sys/resource.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace tak::sim::probe {

inline bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && !(v[0] == '0' && !v[1]);
}
inline const bool kStats = envOn("TAK_SIMSTATS");
inline const bool kPmu = envOn("TAK_PMU");

struct Counters {
    uint64_t vmTicks = 0, vmSkippable = 0, vmEmpty = 0, vmDebtFlushes = 0;
    uint64_t nearScans = 0, nearCells = 0, nearCellsMasked = 0, nearBlocksSkipped = 0;
    uint64_t nearCellsBlockSkippable = 0, nearCellsOutsideDisk = 0, losCalls = 0;
    uint64_t nearCellsTested = 0, nearCellsTestedOutsideDisk = 0;
    uint64_t refreshRects = 0, refreshGradeEvals = 0, refreshRawGrades = 0, refreshCellsRated = 0;
    uint64_t bodyRects = 0, bodyRectsHeap = 0;
    uint64_t compactMoved = 0, passes = 0;
    uint64_t missionScans = 0;   // retailMissionScan calls (4d8370)
};
// Per thread, like the TAK_PHASE accumulators: the server may tick rooms on several threads.
inline thread_local Counters tl;

inline const char* const kPhaseNames[kPhases] = {"tick", "combat", "sep", "explore", "vis", "burn",
                                                 "grid", "scripts", "movement", "nav", "prologue",
                                                 "compact"};

struct PmuSample { uint64_t v[4] = {}; };   // cycles, instructions, L1-miss load stall cycles, LLC misses

#ifdef TAK_PROBE_HAVE_PMU
class Pmu {
public:
    bool ok() const { return ok_; }
    const char* coreType() const { return type_; }
    PmuSample read() const {
        PmuSample s;
        for (int i = 0; i < 4; ++i) s.v[i] = readOne(i);
        return s;
    }
    static Pmu& self() {
        static thread_local Pmu p;
        return p;
    }

private:
    Pmu() {
        // Hybrid parts expose one PMU per core type; the raw stall event differs.
        const int cpu = sched_getcpu();
        int pmuType = -1;
        uint64_t stall = 0;
        for (const char* name : {"cpu_core", "cpu_atom", "cpu"}) {
            const std::string dir = std::string("/sys/bus/event_source/devices/") + name;
            std::ifstream tf(dir + "/type");
            int t = -1;
            if (!(tf >> t)) continue;
            std::ifstream cf(dir + "/cpus");
            std::string cpus;
            if (cf >> cpus && !inList(cpus, cpu)) continue;
            pmuType = t;
            // Atom (Skymont): MEM_BOUND_STALLS_LOAD.ALL (0x34/0x7f). Core (Lion Cove):
            // CYCLE_ACTIVITY.STALLS_L1D_MISS (0xa3/0x0c, cmask 12). Both verified with a
            // pointer chase (91-95% of cycles) against a compute loop (0%).
            stall = std::strcmp(name, "cpu_atom") == 0 ? 0x7f34 : 0x0c000ca3;
            type_ = name;
            break;
        }
        if (pmuType < 0) return;
        const uint64_t cfg[4] = {0x3c, 0xc0, stall, 0x412e};
        ok_ = true;
        for (int i = 0; i < 4; ++i) {
            perf_event_attr a;
            std::memset(&a, 0, sizeof a);
            a.size = sizeof a;
            a.type = uint32_t(pmuType);
            a.config = cfg[i];
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            fd_[i] = int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
            if (fd_[i] < 0) { ok_ = false; continue; }
            page_[i] = mmap(nullptr, size_t(sysconf(_SC_PAGESIZE)), PROT_READ, MAP_SHARED, fd_[i], 0);
            if (page_[i] == MAP_FAILED) { page_[i] = nullptr; ok_ = false; }
        }
        if (!ok_) std::fprintf(stderr, "TAK_PMU: perf_event_open/mmap failed (perf_event_paranoid?)\n");
    }
    static bool inList(const std::string& list, int cpu) {
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            const std::string part = list.substr(i, j - i);
            const size_t dash = part.find('-');
            const int a = std::atoi(part.c_str()), b = dash == std::string::npos ? a : std::atoi(part.c_str() + dash + 1);
            if (cpu >= a && cpu <= b) return true;
            i = j + 1;
        }
        return false;
    }
    // The perf_event_mmap_page self-monitoring protocol: seqlock, then rdpmc.
    uint64_t readOne(int i) const {
        const auto* pg = static_cast<const volatile perf_event_mmap_page*>(page_[i]);
        if (!pg) return 0;
        uint32_t seq;
        uint64_t count;
        do {
            seq = pg->lock;
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
            const uint32_t idx = pg->index;
            count = uint64_t(pg->offset);
            if (pg->cap_user_rdpmc && idx) {
                uint32_t lo, hi;
                __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(idx - 1));
                const unsigned width = pg->pmc_width;
                int64_t pmc = int64_t((uint64_t(hi) << 32) | lo);
                pmc <<= 64 - width;
                pmc >>= 64 - width;
                count += uint64_t(pmc);
            }
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        } while (pg->lock != seq);
        return count;
    }
    int fd_[4] = {-1, -1, -1, -1};
    void* page_[4] = {};
    bool ok_ = false;
    const char* type_ = "none";
};
inline PmuSample pmuRead() { return Pmu::self().read(); }
#else
inline PmuSample pmuRead() { return {}; }
#endif

// D6 spawn-tick allocation: process-wide minor page faults and the change in malloc'd bytes
// inside World::tick (allocMark() at its start, the deltas at the SIMSTATS line), so the
// client's own work between ticks (rendering, capture) is not counted.
struct AllocSample { uint64_t minflt = 0, minfltSelf = 0; int64_t heap = 0; };
// Out of line and cold: World::tick is one very large function, and inlining these into it
// measurably changed its code generation (build-o2 "other" +6% with the switch off).
[[gnu::noinline, gnu::cold]] inline AllocSample allocRead() {
    AllocSample a;
#if defined(__linux__)
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    a.minflt = uint64_t(r.ru_minflt);
    getrusage(RUSAGE_THREAD, &r);
    a.minfltSelf = uint64_t(r.ru_minflt);
#endif
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 m = mallinfo2();
    a.heap = int64_t(m.uordblks + m.hblkhd);
#endif
    return a;
}
inline AllocSample& allocMarkSlot() { static thread_local AllocSample mark; return mark; }
[[gnu::noinline, gnu::cold]] inline void allocMark() { allocMarkSlot() = allocRead(); }
// " minflt=.. minflt_sim=.. heap_grow=.." for the SIMSTATS line: the deltas since allocMark().
[[gnu::noinline, gnu::cold]] inline std::string allocFields() {
    const AllocSample now = allocRead(), &mark = allocMarkSlot();
    char b[96];
    std::snprintf(b, sizeof b, " minflt=%llu minflt_sim=%llu heap_grow=%lld",
                  (unsigned long long)(now.minflt - mark.minflt),
                  (unsigned long long)(now.minfltSelf - mark.minfltSelf), (long long)(now.heap - mark.heap));
    return b;
}

// Per-tick PMU accumulators, one row per phase.
inline thread_local PmuSample tlPmu[kPhases];
inline void pmuReset() { for (auto& s : tlPmu) s = {}; }
inline void pmuAdd(int phase, const PmuSample& from) {
    const PmuSample now = pmuRead();
    for (int i = 0; i < 4; ++i) tlPmu[phase].v[i] += now.v[i] - from.v[i];
}

}  // namespace tak::sim::probe

#define TAK_PROBE(...) do { if (::tak::sim::probe::kStats) { __VA_ARGS__; } } while (0)
#define TAK_PASS() TAK_PROBE(++::tak::sim::probe::tl.passes)
#else
#define TAK_PROBE(...) do {} while (0)
#define TAK_PASS() do {} while (0)
#endif
