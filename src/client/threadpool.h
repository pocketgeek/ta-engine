#pragma once

// Persistent worker pool for data-parallel loops (per-unit animation VM ticks,
// the renderer's geometry/shadow/copy passes). A pass splits [0,count) into
// chunks that participants claim off one atomic word; the calling thread is
// always one of them, and the call returns once every chunk has finished.
// Extracted from client/main.cpp; kept at global scope (and header-only, since
// the dispatch entry points are templates) so its unqualified use sites there are
// unchanged.
//
// Cost-aware dispatch (rendering workstream I): a pass wakes only the workers it
// asks for (`participants`), each worker sleeps on its own condition variable, and
// the caller waits for the CHUNKS, not for the workers: a worker that wakes after
// every chunk was claimed finds nothing and goes back to sleep without the caller
// ever waiting for it. Which items share a chunk and which thread runs it never
// change what a pass computes -- callers write only per-item outputs, so results
// are identical to a serial loop regardless of completion order.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

class ThreadPool {
public:
    // Upper bound on chunks in one pass (and on threads), so the claim word's
    // 32-bit index can never run into its chunk count.
    static constexpr size_t kMaxChunks = size_t(1) << 30;
    static constexpr unsigned kMaxThreads = 1024;

    ThreadPool() : ThreadPool(defaultThreads()) {}
    // A pool of `threads` workers in all, the calling thread included (so threads-1 are
    // spawned). A pool is never shared between two callers that may run at once.
    explicit ThreadPool(unsigned threads) {
        n_ = std::clamp(threads, 1u, kMaxThreads);
        workers_.reserve(n_ - 1);
        for (unsigned i = 1; i < n_; ++i) workers_.push_back(std::make_unique<Worker>());
        for (unsigned i = 0; i < workers_.size(); ++i)
            workers_[i]->thread = std::thread([this, wp = workers_[i].get(), i] {
                tlsPool_ = this;
                tlsParticipant_ = i + 1;
                workerLoop(*wp);
            });
    }
    ~ThreadPool() {
        for (auto& w : workers_) {
            { std::lock_guard<std::mutex> lk(w->m); w->stop = true; }
            w->cv.notify_one();
        }
        for (auto& w : workers_) w->thread.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // The CPUs this process may run on (taskset, containers, cgroup cpusets), not
    // the machine's -- the same rule as the simulation's SimPool. A pool larger
    // than that only adds threads that preempt each other and the caller.
    static unsigned defaultThreads() {
        unsigned cpus = std::thread::hardware_concurrency();
#if defined(__linux__)
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            const int allowed = CPU_COUNT(&set);
            if (allowed > 0) cpus = cpus ? std::min(cpus, unsigned(allowed)) : unsigned(allowed);
        }
#endif
        return cpus ? cpus : 1;
    }

    // Optional dispatch telemetry (render diagnostics). Null = off: a pass then
    // reads no clocks. The caller-thread fields are written by the thread running
    // the pass; worker busy/wake time arrives through atomics and is folded in
    // before the pass returns (a worker that joins after every chunk was claimed
    // counts only as an idle wake, folded at the next timed pass). Changes timing
    // only, never the chunking or the results.
    struct Stats {
        uint64_t dispatches = 0, serial = 0, items = 0;
        uint64_t dispatchNs = 0;   // claim word + waking the requested workers
        uint64_t waitNs = 0;       // caller idle after its own chunks until all finish
        uint64_t busyNs = 0;       // every participant's time running chunks (summed)
        uint64_t wakeNs = 0;       // dispatch -> each worker's first chunk (summed over workers)
        uint64_t wakes = 0;        // workers that ran at least one chunk of a pass
        uint64_t posted = 0;       // workers woken (requested participants minus the caller)
        uint64_t idleWakes = 0;    // woken workers that found every chunk taken
        uint64_t chunks = 0;       // chunks handed out by parallel passes
    };
    void setStats(Stats* stats) { stats_ = stats; }
    unsigned threads() const { return n_; }

    // This thread's participant index in `pool`: 1..threads()-1 on its workers, 0 on
    // any other thread -- i.e. on the pool's caller, the one thread that may drive
    // it (rendering workstream J). Per-participant scratch indexed by it is touched by
    // its worker only inside a chunk, and the caller observes every chunk finished
    // (acquire on the chunk count) before a pass returns and publishes its writes
    // (release on the claim word) before the next one starts, so between passes the
    // caller may inspect, account and trim every slot.
    static unsigned participant(const ThreadPool& pool) {
        return tlsPool_ == &pool ? tlsParticipant_ : 0;
    }

    // Uniform chunks of kChunk items. minParallel: run serially below this count.
    // Wakes at most one worker per chunk. A callsite whose per-item work is tiny
    // must set minParallel high -- e.g. the COB VM tick at ~0.2us/item only breaks
    // even around ~2000 items; renderer passes plan their chunks by estimated cost
    // instead (parallelChunks with client/workschedule.h).
    template <class F>
    void parallelFor(size_t count, F&& f, size_t minParallel = 32) {
        if (count == 0) return;
        if (n_ == 1 || count < minParallel) { runSerial(count, f); return; }
        size_t chunk = kChunk, chunks = (count + kChunk - 1) / kChunk;
        if (chunks > kMaxChunks) { chunk = (count + kMaxChunks - 1) / kMaxChunks; chunks = (count + chunk - 1) / chunk; }
        dispatch(erase(f), &call<F>, count, nullptr, chunk, chunks, unsigned(std::min<size_t>(n_, chunks)));
    }

    // Planned chunks: chunk k covers [bounds[k], bounds[k+1]) (bounds ascending,
    // bounds.front() = 0). `participants` = threads wanted, the caller included;
    // 1 (or a single chunk) runs everything on the caller, in order.
    template <class F>
    void parallelChunks(std::span<const uint32_t> bounds, unsigned participants, F&& f) {
        if (bounds.size() < 2 || bounds.back() == bounds.front()) return;
        const size_t chunks = bounds.size() - 1, count = bounds.back();
        participants = unsigned(std::min<size_t>({size_t(std::max(participants, 1u)), n_, chunks}));
        if (participants <= 1 || chunks > kMaxChunks) { runSerial(count, f); return; }
        dispatch(erase(f), &call<F>, count, bounds.data(), 0, chunks, participants);
    }

private:
    static constexpr size_t kChunk = 8;
    // How long the caller polls for the last chunks before it sleeps (the
    // remaining chunks are already running on other threads by then).
    static constexpr int64_t kSpinNs = 20000;

    struct Worker {
        std::mutex m;
        std::condition_variable cv;
        uint64_t posted = 0;
        bool stop = false;
        std::thread thread;
    };

    template <class F> static void call(void* f, size_t b, size_t e) {
        (*static_cast<std::remove_reference_t<F>*>(f))(b, e);
    }
    template <class F> static void* erase(F& f) {
        return const_cast<void*>(static_cast<const void*>(std::addressof(f)));
    }

    template <class F> void runSerial(size_t count, F& f) {
        if (Stats* const stats = stats_) { ++stats->serial; stats->items += count; }
        f(size_t(0), count);
    }

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    // The job fields are written only while no chunk of any pass is outstanding
    // (the previous pass's chunks all finished; any late claim on its word reads
    // an exhausted index), and published by the release store of the claim word.
    // A thread reads them only after its fetch_add returned an index below the
    // chunk count, i.e. it holds an unfinished chunk, so the pass -- and with it
    // the fields and the caller's functor -- cannot end under it.
    void dispatch(void* fn, void (*callFn)(void*, size_t, size_t), size_t count, const uint32_t* bounds,
                  size_t chunk, size_t chunks, unsigned participants) {
        Stats* const stats = stats_;
        const int64_t start = stats ? nowNs() : 0;
        fn_ = fn; call_ = callFn; count_ = count; bounds_ = bounds; chunk_ = chunk;
        timed_.store(stats != nullptr, std::memory_order_relaxed);
        dispatchedNs_.store(start, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        claim_.store(uint64_t(chunks) << 32, std::memory_order_release);
        unsigned posted = 0;
        for (unsigned i = 0; i + 1 < participants; ++i, ++posted) {
            Worker& w = *workers_[i];
            { std::lock_guard<std::mutex> lk(w.m); ++w.posted; }
            w.cv.notify_one();
        }
        if (stats) {
            stats->dispatchNs += uint64_t(nowNs() - start); ++stats->dispatches; stats->items += count;
            stats->posted += posted; stats->chunks += chunks;
        }
        drain(false);
        const int64_t own = stats ? nowNs() : 0;
        if (done_.load(std::memory_order_acquire) != chunks) {
            const int64_t spinUntil = nowNs() + kSpinNs;
            while (done_.load(std::memory_order_acquire) != chunks && nowNs() < spinUntil) cpuRelax();
            if (done_.load(std::memory_order_acquire) != chunks) {
                std::unique_lock<std::mutex> lk(doneM_);
                doneCv_.wait(lk, [&] { return done_.load(std::memory_order_acquire) == chunks; });
            }
        }
        if (stats) {
            stats->waitNs += uint64_t(nowNs() - own);
            stats->busyNs += busyNs_.exchange(0, std::memory_order_relaxed);
            stats->wakeNs += wakeNs_.exchange(0, std::memory_order_relaxed);
            stats->wakes += wakes_.exchange(0, std::memory_order_relaxed);
            stats->idleWakes += idleWakes_.exchange(0, std::memory_order_relaxed);
        }
    }

    void drain(bool worker) {
        bool joined = false;
        for (;;) {
            const uint64_t c = claim_.fetch_add(1, std::memory_order_acq_rel);
            const size_t k = size_t(c & 0xffffffffu), chunks = size_t(c >> 32);
            if (k >= chunks) break;
            const bool timed = timed_.load(std::memory_order_relaxed);
            const int64_t t0 = timed ? nowNs() : 0;
            if (timed && worker && !joined) {
                wakeNs_.fetch_add(uint64_t(std::max<int64_t>(0, t0 - dispatchedNs_.load(std::memory_order_relaxed))),
                                  std::memory_order_relaxed);
                wakes_.fetch_add(1, std::memory_order_relaxed);
            }
            joined = true;
            size_t b, e;
            if (bounds_) { b = bounds_[k]; e = bounds_[k + 1]; }
            else { b = k * chunk_; e = std::min(b + chunk_, count_); }
            call_(fn_, b, e);
            if (timed) busyNs_.fetch_add(uint64_t(nowNs() - t0), std::memory_order_relaxed);
            // The last chunk wakes the caller. After this increment the pass may end:
            // only pool members are touched from here on.
            if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == chunks) {
                std::lock_guard<std::mutex> lk(doneM_);
                doneCv_.notify_one();
            }
        }
        if (worker && !joined) idleWakes_.fetch_add(1, std::memory_order_relaxed);
    }

    void workerLoop(Worker& w) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(w.m);
                w.cv.wait(lk, [&] { return w.stop || w.posted != seen; });
                if (w.stop) return;
                seen = w.posted;
            }
            drain(true);
        }
    }

    static inline thread_local const ThreadPool* tlsPool_ = nullptr;
    static inline thread_local unsigned tlsParticipant_ = 0;

    unsigned n_ = 1;
    std::vector<std::unique_ptr<Worker>> workers_;
    // Claim word: chunk count (high 32 bits) | next chunk index (low 32 bits). Each
    // participant ends its pass with one failed claim, so the index stays far from
    // the count field.
    std::atomic<uint64_t> claim_{0};
    std::atomic<size_t> done_{0};
    void* fn_ = nullptr;
    void (*call_)(void*, size_t, size_t) = nullptr;
    size_t count_ = 0, chunk_ = 0;
    const uint32_t* bounds_ = nullptr;
    std::mutex doneM_;
    std::condition_variable doneCv_;
    Stats* stats_ = nullptr;
    std::atomic<bool> timed_{false};
    std::atomic<int64_t> dispatchedNs_{0};
    std::atomic<uint64_t> busyNs_{0}, wakeNs_{0}, wakes_{0}, idleWakes_{0};
};
