// Cost-planned worker-pool passes (rendering workstream I).
//
// WorkPlan (client/workschedule.h): chunks are contiguous, cover every item once,
// follow the cost estimates, and the participant count follows the estimate and
// every cap; the plan is a pure function of its inputs.
//
// ThreadPool (client/threadpool.h): planned and uniform passes run every item
// exactly once and produce the serial result whatever the completion order; a
// pass wakes only the workers it asks for; the caller returns as soon as every
// chunk finished, and a worker that wakes late never touches a finished pass
// (back-to-back passes over short-lived functors and buffers -- run this test
// under ASan/TSan to check that part); pools of one thread run serially.

#include "client/threadpool.h"
#include "client/workschedule.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

using tak::render::SchedulePolicy;
using tak::render::WorkPlan;

bool wellFormed(const WorkPlan& plan, size_t count) {
    const auto b = plan.bounds();
    if (b.empty() || b.front() != 0 || b.back() != count) return false;
    for (size_t k = 1; k < b.size(); ++k)
        if (b[k] <= b[k - 1]) return false;
    return plan.items() == count;
}

// f(i) for every item, written to its own slot: the serial reference.
uint64_t value(size_t i) { return uint64_t(i) * 0x9e3779b97f4a7c15ull ^ (i >> 3); }
}  // namespace

int main() {
    {   // Plan shape: contiguous, complete, cost-following, deterministic.
        SchedulePolicy p{.serialBelowNs = 30000, .workerNs = 40000, .chunkNs = 10000};
        WorkPlan plan;
        plan.begin(p);
        for (int i = 0; i < 100; ++i) plan.add(1000);   // 100 us in 1 us items
        plan.finish(8);
        check(wellFormed(plan, 100), "uniform plan covers every item once");
        check(plan.chunks() == 10, "10 us chunks of 1 us items");
        check(plan.participants() == 3, "100 us / 40 us per participant -> 3");
        check(plan.estimateNs() == 100000, "estimate is the sum");

        plan.begin(p);
        for (int i = 0; i < 20; ++i) plan.add(1000);
        plan.finish(8);
        check(wellFormed(plan, 20) && plan.participants() == 1, "20 us of work runs on the caller");
        // Below the serial threshold even when the per-participant rate asks for more.
        SchedulePolicy eager{.serialBelowNs = 30000, .workerNs = 5000, .chunkNs = 1000};
        plan.begin(eager);
        for (int i = 0; i < 25; ++i) plan.add(1000);
        plan.finish(8);
        check(plan.chunks() == 25 && plan.participants() == 1, "25 us under a 30 us threshold stays serial");
        plan.begin(eager);
        for (int i = 0; i < 35; ++i) plan.add(1000);
        plan.finish(8);
        check(plan.participants() == 7, "35 us over the threshold: 7 participants of 5 us");

        // One expensive item among cheap ones gets a chunk of its own; cheap runs share.
        plan.begin(p);
        for (int i = 0; i < 50; ++i) plan.add(i == 25 ? 200000 : 500);
        plan.finish(24);
        check(wellFormed(plan, 50), "mixed plan well formed");
        bool alone = false;
        for (size_t k = 0; k + 1 < plan.bounds().size(); ++k)
            alone = alone || (plan.bounds()[k] <= 25 && plan.bounds()[k + 1] == 26);
        check(alone, "an expensive item closes its chunk");
        check(plan.participants() == std::min<size_t>(plan.chunks(), 6), "participants follow cost, capped by chunks");

        // Caps: pool size, chunk count and the policy's own cap.
        plan.begin(p);
        for (int i = 0; i < 1000; ++i) plan.add(5000);   // 5 ms
        plan.finish(4);
        check(plan.participants() == 4, "capped by the pool");
        SchedulePolicy capped = p;
        capped.maxParticipants = 2;
        plan.begin(capped);
        for (int i = 0; i < 1000; ++i) plan.add(5000);
        plan.finish(24);
        check(plan.participants() == 2, "capped by the policy");
        plan.begin(p);
        for (int i = 0; i < 3; ++i) plan.add(1e6);
        plan.finish(24);
        check(plan.chunks() == 3 && plan.participants() == 3, "capped by the chunk count");

        // Free items (zero/negative estimates) never close a chunk and never wake a worker.
        plan.begin(p);
        for (int i = 0; i < 5000; ++i) plan.add(i % 2 ? 0.0 : -1.0);
        plan.finish(24);
        check(wellFormed(plan, 5000) && plan.chunks() == 1 && plan.participants() == 1, "free items: one serial chunk");

        // Empty plan.
        plan.begin(p);
        plan.finish(24);
        check(plan.chunks() == 0 && plan.participants() == 1 && plan.bounds().size() == 1, "empty plan");

        // Chunk cap: the last chunk absorbs the rest.
        SchedulePolicy tiny{.serialBelowNs = 0, .workerNs = 1, .chunkNs = 1};
        plan.begin(tiny);
        for (size_t i = 0; i < WorkPlan::kMaxChunks + 500; ++i) plan.add(1);
        plan.finish(24);
        check(wellFormed(plan, WorkPlan::kMaxChunks + 500) && plan.chunks() == WorkPlan::kMaxChunks + 1,
              "chunk count bounded");

        // Deterministic: same estimates, same plan.
        std::mt19937 rng(7);
        std::vector<double> costs(3000);
        for (auto& c : costs) c = double(rng() % 50000);
        WorkPlan a, b;
        a.begin(p); b.begin(p);
        for (double c : costs) { a.add(c); b.add(c); }
        a.finish(8); b.finish(8);
        check(std::vector<uint32_t>(a.bounds().begin(), a.bounds().end()) ==
              std::vector<uint32_t>(b.bounds().begin(), b.bounds().end()) && a.participants() == b.participants(),
              "plan is a pure function of the estimates");

        // Packed units (addUnits) plan exactly like one add() per item.
        for (int round = 0; round < 50; ++round) {
            std::vector<uint16_t> units(1 + rng() % 5000);
            for (auto& u : units) u = uint16_t(rng() % (round % 5 == 0 ? 3 : 700));
            const double unitNs = 64;
            SchedulePolicy q{.serialBelowNs = double(rng() % 50000), .workerNs = 1000 + double(rng() % 50000),
                             .chunkNs = 100 + double(rng() % 20000)};
            WorkPlan perItem, packed;
            perItem.begin(q); packed.begin(q);
            const size_t split = units.size() / 3;   // also mixed with add() before it
            for (size_t i = 0; i < split; ++i) { perItem.add(units[i] * unitNs); packed.add(units[i] * unitNs); }
            for (size_t i = split; i < units.size(); ++i) perItem.add(units[i] * unitNs);
            packed.addUnits(std::span<const uint16_t>(units).subspan(split), unitNs);
            perItem.finish(8); packed.finish(8);
            if (std::vector<uint32_t>(perItem.bounds().begin(), perItem.bounds().end()) !=
                    std::vector<uint32_t>(packed.bounds().begin(), packed.bounds().end()) ||
                perItem.participants() != packed.participants() || perItem.estimateNs() != packed.estimateNs()) {
                check(false, "addUnits plans like add() per item");
                break;
            }
        }
    }
    {   // Pool: planned passes equal the serial loop, whatever the order of completion.
        ThreadPool pool(6);
        std::mt19937 rng(11);
        bool same = true, once = true;
        for (int round = 0; round < 300 && same && once; ++round) {
            const size_t count = 1 + rng() % 4000;
            SchedulePolicy p{.serialBelowNs = double(rng() % 3) * 1000, .workerNs = 2000 + double(rng() % 20000),
                             .chunkNs = 500 + double(rng() % 5000)};
            WorkPlan plan;
            plan.begin(p);
            for (size_t i = 0; i < count; ++i) plan.add(double(rng() % 3000));
            plan.finish(pool.threads());
            std::vector<uint64_t> out(count, 0);
            std::vector<std::atomic<int>> visits(count);
            const bool jitter = round % 7 == 0;
            pool.parallelChunks(plan.bounds(), plan.participants(), [&](size_t b, size_t e) {
                if (jitter && (b * 31) % 5 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
                for (size_t i = b; i < e; ++i) { out[i] = value(i); visits[i].fetch_add(1); }
            });
            for (size_t i = 0; i < count; ++i) {
                same = same && out[i] == value(i);
                once = once && visits[i].load() == 1;
            }
        }
        check(same, "planned passes equal the serial result");
        check(once, "every item runs exactly once");
    }
    {   // Uniform passes (parallelFor) keep their contract: serial below minParallel.
        ThreadPool pool(4);
        ThreadPool::Stats stats;
        pool.setStats(&stats);
        std::vector<int> out(1000, 0);
        pool.parallelFor(out.size(), [&](size_t b, size_t e) { for (size_t i = b; i < e; ++i) out[i] = int(i); });
        bool ok = true;
        for (size_t i = 0; i < out.size(); ++i) ok = ok && out[i] == int(i);
        check(ok, "uniform pass result");
        pool.parallelFor(10, [&](size_t b, size_t e) { check(b == 0 && e == 10, "serial pass is one call"); });
        pool.parallelFor(24, [&](size_t, size_t) {}, 25);
        check(stats.dispatches == 1 && stats.serial == 2 && stats.items == 1034, "uniform dispatch/serial counts");
        check(stats.posted == 3 && stats.chunks == 125, "uniform pass wakes min(pool, chunks) - 1 workers");
        // 16 items -> 2 chunks -> at most one worker woken.
        pool.parallelFor(16, [&](size_t, size_t) {}, 1);
        check(stats.posted == 4, "two chunks wake one worker");
        pool.setStats(nullptr);
    }
    {   // Only the requested workers are woken; wakes never exceed posts + stragglers.
        ThreadPool pool(8);
        ThreadPool::Stats stats;
        pool.setStats(&stats);
        std::vector<uint32_t> bounds;
        for (uint32_t k = 0; k <= 64; ++k) bounds.push_back(k * 10);
        std::atomic<int> total{0};
        for (unsigned want : {1u, 2u, 3u, 8u, 20u}) {
            const ThreadPool::Stats before = stats;
            pool.parallelChunks(bounds, want, [&](size_t b, size_t e) { total.fetch_add(int(e - b)); });
            const unsigned expect = std::min(want, 8u);
            check(stats.posted - before.posted == (expect > 1 ? expect - 1 : 0), "posts = participants - 1");
            check(expect > 1 ? stats.dispatches == before.dispatches + 1 : stats.serial == before.serial + 1,
                  "one participant runs serially");
        }
        check(total.load() == 5 * 640, "every item of every pass ran");
        // Every worker that ran a chunk was woken for this or an earlier pass.
        check(stats.wakes <= stats.posted, "no worker runs without a post");
        check(stats.busyNs > 0, "busy time recorded");
        pool.setStats(nullptr);
    }
    {   // Lifetime: back-to-back passes over short-lived functors/buffers. A late
        // worker that touched a finished pass would write a freed buffer (ASan)
        // or race the next pass's setup (TSan), or corrupt the next pass's result.
        ThreadPool pool(8);
        std::mt19937 rng(3);
        bool ok = true;
        for (int round = 0; round < 20000 && ok; ++round) {
            const size_t count = 1 + rng() % 64;
            auto buf = std::make_unique<uint64_t[]>(count);
            std::vector<uint32_t> bounds{0};
            while (bounds.back() < count) bounds.push_back(uint32_t(std::min<size_t>(count, bounds.back() + 1 + rng() % 9)));
            const uint64_t salt = rng();
            pool.parallelChunks(bounds, 1 + rng() % 8, [&, salt](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) buf[i] = value(i) ^ salt;
            });
            for (size_t i = 0; i < count; ++i) ok = ok && buf[i] == (value(i) ^ salt);
            if (round % 3 == 0) pool.parallelFor(count, [&, salt](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) buf[i] = value(i) + salt;
            }, 1);
        }
        check(ok, "20000 back-to-back passes, every result complete at return");
    }
    {   // Pools of one thread, and destruction with sleeping workers.
        ThreadPool one(1);
        std::thread::id seen{};
        bool sameThread = true;
        const std::vector<uint32_t> bounds{0, 5, 10, 15};
        one.parallelChunks(bounds, 8, [&](size_t, size_t) {
            sameThread = sameThread && std::this_thread::get_id() == std::this_thread::get_id();
            seen = std::this_thread::get_id();
        });
        check(seen == std::this_thread::get_id() && sameThread, "single-thread pool runs on the caller");
        for (int i = 0; i < 50; ++i) { ThreadPool p(5); p.parallelFor(100, [](size_t, size_t) {}, 1); }
        check(ThreadPool::defaultThreads() >= 1, "default size is at least one thread");
    }
    {   // Participant indices (workstream J): per-participant scratch slots. Two chunks
        // running at once never share an index; the caller is 0, and another pool's
        // workers are 0 for this pool. Slots grown inside chunks and trimmed by the
        // caller between passes -- run under TSan: the pass boundaries order them.
        ThreadPool pool(6), other(3);
        check(ThreadPool::participant(pool) == 0, "the caller is participant 0");
        std::vector<std::atomic<int>> inUse(pool.threads());
        std::vector<std::vector<uint64_t>> slots(pool.threads());
        std::atomic<bool> clash{false}, range{true}, foreign{true}, otherWorkers{false};
        std::set<unsigned> seen;
        std::mutex seenMu;
        std::mt19937 rng(11);
        size_t trimmed = 0;
        for (int round = 0; round < 3000; ++round) {
            std::vector<uint32_t> bounds{0};
            const size_t count = 64 + rng() % 400;
            while (bounds.back() < count) bounds.push_back(uint32_t(std::min<size_t>(count, bounds.back() + 1 + rng() % 7)));
            pool.parallelChunks(bounds, 1 + rng() % 6, [&](size_t b, size_t e) {
                const unsigned p = ThreadPool::participant(pool);
                if (p >= pool.threads()) { range = false; return; }
                if (inUse[p].fetch_add(1) != 0) clash = true;
                auto& slot = slots[p];
                for (size_t i = b; i < e; ++i) slot.push_back(value(i));
                if (round % 500 == 0) { std::lock_guard<std::mutex> lk(seenMu); seen.insert(p); }
                inUse[p].fetch_sub(1);
            });
            if (round % 7 == 0)
                other.parallelFor(64, [&](size_t, size_t) {
                    if (ThreadPool::participant(pool) != 0) foreign = false;
                    if (ThreadPool::participant(other) != 0) otherWorkers = true;
                    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
                    while (std::chrono::steady_clock::now() < until) {}
                }, 1);
            // Between passes the caller owns every slot: account and trim them.
            for (auto& slot : slots) {
                if (round % 13 == 0) { trimmed += slot.capacity(); std::vector<uint64_t>().swap(slot); }
                else slot.clear();
            }
        }
        check(!clash.load(), "two running chunks never share a participant index");
        check(range.load(), "participant indices stay below threads()");
        check(foreign.load(), "another pool's workers are participant 0 here");
        check(otherWorkers.load(), "fixture: the other pool's workers ran chunks");
        check(seen.size() > 1, "workers ran with their own indices");
        check(trimmed > 0, "fixture: slots were trimmed between passes");
    }
    if (failures) return EXIT_FAILURE;
    std::printf("PASS: work plans and cost-planned pool passes\n");
    return EXIT_SUCCESS;
}
