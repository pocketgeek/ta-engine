// navbudget_test -- the field scheduler's service policy (sim/navbudget.h; Legion 10,000 stage 4).
//
//   navbudget_test
//
// Drives navDrr, navPromote and NavAdmit -- the code LegionNavigator's serveFields runs -- with
// synthetic jobs and requests, the way serveFields does (classes in order, each by deficit round robin
// over a window of its oldest jobs with its cursor kept across ticks, the refresh classes inside the
// refresh allowance, promotion by the time since a job's last service), and checks service and the
// longest time anything waits under CONTINUOUS load, not throughput:
//
// 1. Fair share: the jobs of a full window share the quota within two slices of each other, and none
//    goes more than one rotation (ceil(window x slice / quota) + 1 ticks) without work.
// 2. Oldest first across windows: of 40 equal jobs in one class, none finishes before a job more than a
//    window older than it.
// 3. Continuous bursty load near capacity in all four classes (deterministic LCG: 90% of the quota on
//    average, bursts of three quotas): every job completes, and no job waits longer for work (from its
//    arrival, and between slices) than 3 x kAgeStep (promotion to the top class) plus the largest backlog
//    one burst leaves (its work over the quota) plus one rotation.
// 4. Age promotion: with the quota saturated by waiting jobs (overload), a lone refresh job still gets
//    work once it has aged up to the top class, and again after each such interval.
// 5. A shared job is one job: two groups on one field do not double its slices (the job list is keyed
//    by the field).
// 6. The bounded fit scan: a request too large for the free room is stepped over (at most kFitScan
//    refusals a tick) while smaller ones start, reserves after kReserveAge ticks, and from then on nothing
//    that arrived after it takes room before it does -- but requests behind the stopped scan that only
//    share an existing field (no room) still start (serveFields: shareOnly behind a stopped scan).
// 7. The scan bounds (kWaitFitScan in the Waiting class, kFitScan below it), and a reservation stops its
//    own class and every later one.
#include "sim/navbudget.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

using namespace tak::sim;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-74s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}
struct Lcg {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    uint32_t below(uint32_t n) { return next() % n; }
};

struct Job {
    uint64_t key = 0;
    NavClass base = NavClass::Waiting;
    int64_t deficit = 0;
    uint32_t lastServed = 0;
    uint64_t remaining = 0;     // ~0: endless
    uint64_t served = 0;
    uint32_t maxGap = 0;
    bool done = false;
};
bool refreshClass(NavClass c) { return c == NavClass::AwareRepair || c == NavClass::StaleRefresh; }

// One tick of serveFields' job service over `jobs` (keys ascending).
struct Scheduler {
    std::array<NavCursor, size_t(NavClass::Count)> cursor{};
    uint64_t visits = 0;
    void tick(std::map<uint64_t, Job>& jobs, uint32_t now) {
        uint64_t budget = NavBudget::kFieldQuota, refresh = NavBudget::kRefreshQuota;
        std::map<uint64_t, NavClass> cls;
        for (auto& [k, j] : jobs) if (!j.done) cls[k] = navPromote(j.base, now - j.lastServed);
        for (size_t c = 0; c < size_t(NavClass::Count); ++c) {
            struct Ref { uint64_t key; int64_t* deficit; Job* job; };
            std::vector<Ref> ring;
            for (auto& [k, j] : jobs) if (!j.done && cls[k] == NavClass(c)) ring.push_back({k, &j.deficit, &j});
            navDrr(ring, refreshClass(NavClass(c)) ? NavBudget::kRefreshWindow : NavBudget::kWindow, cursor[c], budget,
                   refreshClass(NavClass(c)) ? &refresh : nullptr,
                   [&](Ref& r, uint64_t allow) -> NavVisit {
                       Job& j = *r.job;
                       ++visits;
                       const uint64_t spent = std::min(allow, j.remaining);
                       j.remaining -= spent;
                       if (spent) {
                           j.maxGap = std::max(j.maxGap, now - j.lastServed);
                           j.lastServed = now;
                           j.served += spent;
                       }
                       if (!j.remaining) j.done = true;
                       return {spent, !j.done};
                   });
        }
    }
};

void fairness() {
    std::printf("1. fair share within a window\n");
    std::map<uint64_t, Job> jobs;
    const uint64_t n = NavBudget::kWindow;
    for (uint64_t k = 1; k <= n; ++k) jobs[k] = Job{k, NavClass::FirstRoute, 0, 0, ~0ull};
    Scheduler s;
    for (uint32_t t = 1; t <= 1000; ++t) s.tick(jobs, t);
    uint64_t lo = ~0ull, hi = 0;
    uint32_t gap = 0;
    for (auto& [k, j] : jobs) { lo = std::min(lo, j.served); hi = std::max(hi, j.served); gap = std::max(gap, j.maxGap); }
    const uint32_t bound = uint32_t((n * NavBudget::kSlice + NavBudget::kFieldQuota - 1) / NavBudget::kFieldQuota) + 1;
    std::printf("     served min %llu max %llu, longest gap %u ticks (bound %u)\n", (unsigned long long)lo,
                (unsigned long long)hi, gap, bound);
    check(hi - lo <= 2 * uint64_t(NavBudget::kSlice), "every job within two slices of every other");
    check(gap <= bound, "no job waits longer than one rotation of the window");
}

void oldestFirst() {
    std::printf("2. oldest first across windows\n");
    std::map<uint64_t, Job> jobs;
    for (uint64_t k = 1; k <= 40; ++k) jobs[k] = Job{k, NavClass::FirstRoute, 0, 0, 1'500'000};
    std::map<uint64_t, uint32_t> doneAt;
    Scheduler s;
    for (uint32_t t = 1; t <= 400 && doneAt.size() < jobs.size(); ++t) {
        s.tick(jobs, t);
        for (auto& [k, j] : jobs) if (j.done && !doneAt.count(k)) doneAt[k] = t;
    }
    bool ordered = doneAt.size() == jobs.size();
    for (auto& [a, ta] : doneAt)
        for (auto& [b, tb] : doneAt)
            if (a + NavBudget::kWindow < b && ta > tb) ordered = false;
    std::printf("     finished %zu of %zu; first at tick %u, last at tick %u\n", doneAt.size(), jobs.size(),
                doneAt.empty() ? 0 : doneAt.begin()->second, doneAt.empty() ? 0 : doneAt.rbegin()->second);
    check(ordered, "no job finishes before one more than a window older than it");
}

// Continuous load: arrivals every tick into all four classes (bursty, 90% of the quota on average), or a
// flood of waiting jobs (overload) with one lone refresh job; live jobs capped at the navigator's 1,024.
void load(bool waitingFlood) {
    std::printf(waitingFlood ? "4. age promotion under a waiting flood\n" : "3. continuous bursty load, four classes\n");
    std::map<uint64_t, Job> jobs;
    Scheduler s;
    Lcg r{waitingFlood ? 7ull : 42ull};
    uint64_t nextKey = 1;
    // A job's lastServed starts at its arrival, so maxGap covers the wait for its first slice too.
    std::array<uint32_t, size_t(NavClass::Count)> worst{};
    std::array<uint64_t, size_t(NavClass::Count)> doneN{};
    uint64_t live = 0, arrivedWork = 0, burstMax = 0;
    const uint32_t ticks = 6000;
    if (waitingFlood) { jobs[nextKey] = Job{nextKey, NavClass::StaleRefresh, 0, 0, ~0ull}; ++nextKey; ++live; }
    for (uint32_t t = 1; t <= ticks; ++t) {
        // Waiting flood: three jobs a tick of 200k..400k (about 2.3 quotas). Bursty: a burst of three
        // quotas every 50 ticks, and 0.84 quotas a tick otherwise (0.9 quotas a tick on average).
        uint64_t want = waitingFlood ? 0 : (t % 50 == 0 ? 3 * NavBudget::kFieldQuota : NavBudget::kFieldQuota * 84 / 100);
        uint64_t got = 0;
        for (int i = 0; live < 1024; ++i) {
            const uint64_t size = waitingFlood ? 200'000 + r.below(200'000) : 20'000 + r.below(300'000);
            if (waitingFlood ? i >= 3 : got + size > want) break;
            Job j;
            j.key = nextKey++;
            j.base = waitingFlood ? NavClass::Waiting : NavClass(r.below(4));
            j.remaining = size;
            j.lastServed = t;
            arrivedWork += j.remaining;
            got += j.remaining;
            jobs[j.key] = j;
            ++live;
        }
        burstMax = std::max(burstMax, got);
        s.tick(jobs, t);
        for (auto it = jobs.begin(); it != jobs.end();) {
            Job& j = it->second;
            const size_t c = size_t(j.base);
            if (!j.done) {
                worst[c] = std::max(worst[c], std::max(j.maxGap, t - j.lastServed));   // an open gap counts too
                ++it;
                continue;
            }
            worst[c] = std::max(worst[c], j.maxGap);
            ++doneN[c];
            --live;
            it = jobs.erase(it);
        }
    }
    const uint32_t rotation = uint32_t((NavBudget::kWindow * NavBudget::kSlice + NavBudget::kFieldQuota - 1) / NavBudget::kFieldQuota) + 1;
    const uint32_t backlog = uint32_t((burstMax + NavBudget::kFieldQuota - 1) / NavBudget::kFieldQuota);
    const uint32_t bound = 3 * NavBudget::kAgeStep + backlog + rotation;
    std::printf("     arrived %.2f quotas/tick, live %llu at the end; longest gap waiting %u first %u aware %u stale %u "
                "(bound %u); done %llu/%llu/%llu/%llu\n",
                double(arrivedWork) / ticks / NavBudget::kFieldQuota, (unsigned long long)live, worst[0], worst[1],
                worst[2], worst[3], bound, (unsigned long long)doneN[0], (unsigned long long)doneN[1],
                (unsigned long long)doneN[2], (unsigned long long)doneN[3]);
    if (waitingFlood) {
        const Job* stale = jobs.count(1) ? &jobs.at(1) : nullptr;
        check(stale && stale->served > 0, "the lone stale refresh still gets work under the flood");
        check(stale && stale->maxGap >= 2 * NavBudget::kAgeStep && stale->maxGap <= 3 * NavBudget::kAgeStep + rotation,
              "... after aging to the top class, and within that time plus one rotation");
    } else {
        bool ok = true, all = true;
        for (auto w : worst) ok &= w <= bound;
        for (auto d : doneN) all &= d > 0;
        check(ok, "every class's longest wait within 3 x kAgeStep + one burst's backlog + one rotation");
        check(all && live < 1024, "jobs of every class complete; the backlog never reaches the 1,024-job cap");
    }
}

void sharedJob() {
    std::printf("5. a shared field is one job\n");
    // Two groups subscribe to job 1; jobs 2 and 3 have one group each. The job list is keyed by the
    // field (serveFields' subscribe), so job 1 appears once in its class's ring.
    std::map<uint64_t, Job> jobs;
    std::map<uint64_t, int> subscribers{{1, 2}, {2, 1}, {3, 1}};
    for (auto [k, n] : subscribers) jobs[k] = Job{k, NavClass::FirstRoute, 0, 0, ~0ull};
    Scheduler s;
    for (uint32_t t = 1; t <= 300; ++t) s.tick(jobs, t);
    const double a = double(jobs[1].served), b = double(jobs[2].served), c = double(jobs[3].served);
    std::printf("     served: shared %.0f, single %.0f and %.0f\n", a, b, c);
    check(a <= b + NavBudget::kSlice * 2 && a <= c + NavBudget::kSlice * 2, "the shared job gets no more than a single one");
}

void fitScan() {
    std::printf("6. the bounded fit scan and reservation\n");
    // Room for 20 units; live items hold their size and leave 25 ticks after they start. One small
    // request (size 1) arrives every tick, more than the room serves, so it stays full and the backlog
    // grows; every fourth tick a request that can share an existing field (size 0) arrives too; one large
    // request (size 10) arrives at t = 40, when the room is full.
    struct Live { int size; uint32_t until; };
    struct Req { uint32_t since; int id, size; uint32_t blocked = NavAdmit::kNever; };
    std::vector<Live> live;
    std::vector<Req> reqs;
    int used = 0, nextId = 1, bigId = -1;
    uint32_t bigStarted = 0, smallWhileBlocked = 0, maxRefusalsInTick = 0, reservedAt = 0, overtook = 0;
    uint32_t sharesWhileReserved = 0, sharesWaited = 0;
    bool reservedSeen = false;
    for (uint32_t t = 1; t <= 600 && !bigStarted; ++t) {
        for (auto it = live.begin(); it != live.end();)
            if (it->until <= t) { used -= it->size; it = live.erase(it); } else ++it;
        reqs.push_back({t, nextId++, 1});
        if (t % 4 == 0) reqs.push_back({t, nextId++, 0});
        if (t == 40) { reqs.push_back({t, nextId, 10}); bigId = nextId++; }
        std::sort(reqs.begin(), reqs.end(), [](const Req& a, const Req& b) { return a.since != b.since ? a.since < b.since : a.id < b.id; });
        NavAdmit admit;
        admit.waiting = false;   // a class below Waiting: the narrow scan, and a reservation stops it
        uint32_t refusals = 0;
        for (auto it = reqs.begin(); it != reqs.end();) {
            if (admit.stopped()) {   // behind the stopped scan only a share starts (it takes no room)
                if (it->size == 0) {
                    NavAdmit::admitted(it->blocked);
                    if (reservedAt) { ++sharesWhileReserved; sharesWaited = std::max(sharesWaited, t - it->since); }
                    it = reqs.erase(it);
                } else ++it;
                continue;
            }
            if (used + it->size <= 20) {
                NavAdmit::admitted(it->blocked);
                used += it->size;
                live.push_back({it->size, t + 25});
                if (it->id == bigId) bigStarted = t;
                else if (bigId > 0 && !bigStarted) {
                    ++smallWhileBlocked;
                    if (reservedAt && it->since > 40) ++overtook;   // a later request started past the reservation
                }
                it = reqs.erase(it);
                continue;
            }
            ++refusals;
            admit.refuse(it->blocked, t);
            if (admit.reserved && it->id == bigId && !reservedSeen) { reservedSeen = true; reservedAt = t; }
            ++it;
        }
        maxRefusalsInTick = std::max(maxRefusalsInTick, refusals);
    }
    std::printf("     the large request started at tick %u (arrived 40, reserved at %u); small starts meanwhile %u, "
                "later ones past the reservation %u; most refusals in one tick %u; shares while reserved %u "
                "(longest wait %u)\n", bigStarted, reservedAt, smallWhileBlocked, overtook, maxRefusalsInTick,
                sharesWhileReserved, sharesWaited);
    check(maxRefusalsInTick <= uint32_t(NavBudget::kFitScan), "at most kFitScan refusals a tick");
    check(smallWhileBlocked > 0, "smaller requests start while the large one waits (no head-of-line block)");
    check(reservedSeen && reservedAt == 40 + NavBudget::kReserveAge, "the large request reserved after kReserveAge ticks");
    check(overtook == 0, "no request that came after it takes room once it has reserved");
    check(sharesWhileReserved > 0 && sharesWaited <= 1, "shares behind the reservation still start, at once");
    check(bigStarted > 40, "the large request waited for room (the scenario saturates it)");
    check(bigStarted && bigStarted < 600, "... and it starts");
}
}  // namespace

void waitingReservation() {
    std::printf("7. the Waiting class's scan and a reservation\n");
    // A Waiting request refused since t = 0 reserves at t = 200: nothing after it starts that tick, in
    // its class or a later one.
    NavAdmit admit;
    uint32_t bigBlocked = 0;
    admit.refuse(bigBlocked, 200);
    const bool stoppedHere = admit.stopped();
    admit.nextClass();
    check(admit.reserved && stoppedHere && admit.stopped(), "a reservation stops its own class and every later one");
    // The narrow scan below the Waiting class, the wide one in it.
    NavAdmit b;
    b.nextClass();
    int n = 0;
    for (; n < 100 && !b.stopped(); ++n) { uint32_t t = NavAdmit::kNever; b.refuse(t, 0); }
    check(n == NavBudget::kFitScan, "a class below Waiting steps over at most kFitScan refusals");
    NavAdmit c;
    int m = 0;
    for (; m < 1000 && !c.stopped(); ++m) { uint32_t t = NavAdmit::kNever; c.refuse(t, 0); }
    check(m == NavBudget::kWaitFitScan, "the Waiting class steps over at most kWaitFitScan refusals");
}

int main() {
    fairness();
    oldestFirst();
    load(false);
    load(true);
    sharedJob();
    fitScan();
    waitingReservation();
    if (failures) { std::printf("navbudget_test: %d FAILED\n", failures); return 1; }
    std::printf("navbudget_test: all passed\n");
    return 0;
}
