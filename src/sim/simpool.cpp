#include "simpool.h"
#include "sim/simcpus.h"

#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace tak::sim {

namespace {
inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
// How long an idle worker polls for the next job before it sleeps. The jobs of
// one tick come back to back; between ticks the pool sleeps.
constexpr auto kSpin=std::chrono::microseconds(20);
} // namespace

unsigned SimPool::usableCpus() {
    unsigned cpus=std::thread::hardware_concurrency();
#if defined(__linux__)
    // The CPUs this process may run on (taskset, containers), not the machine's.
    // Its main thread's: the calling thread's own (pid 0) is one CPU once it is
    // pinned (sim/simcpus.h).
    cpu_set_t set;
    if (sched_getaffinity(getpid(),sizeof(set),&set)==0) cpus=unsigned(CPU_COUNT(&set));
#endif
    return std::max(1u,cpus);
}

int SimPool::debugWorkers() {
#ifndef NDEBUG
    if (const char* e=std::getenv("TAK_SIM_WORKERS"))
        return std::clamp(std::atoi(e),0,int(kMaxWorkers));
#endif
    return -1;
}

unsigned SimPool::defaultWorkers() {
    if (const int debug=debugWorkers();debug>=0) return unsigned(debug);
    return std::min(3u,usableCpus()/8);
}

SimPool::Session SimPool::session(int choice,const Cpus& cpus,bool localServer,bool* debugAuto) {
    Session s=sessionThreads(choice,cpus,localServer);
    const int debug=choice<=0 ? debugWorkers() : -1;
    if (debugAuto) *debugAuto=debug>=0;
    if (debug>=0) {
        s.game=unsigned(debug)+1;
        s.server=localServer ? s.game : 0;
        s.total=s.game+s.server;
    }
    return s;
}

SimPool::HandOut SimPool::handOut(const std::vector<int>& order,const Session& session,bool localServer,bool pin) {
    HandOut out;
    const size_t count=pin ? std::min<size_t>(session.total,order.size()) : 0;
    for (size_t i=0;i<count;++i) {
        const bool server=localServer && (i&1);
        std::vector<int>& to=server ? out.server : out.game;
        if (to.size()<(server ? session.server : session.game)) to.push_back(order[i]);
    }
    return out;
}

SimPool::SimPool(unsigned workers,const std::vector<int>* cpus)
    :pin_(cpus!=nullptr),cpus_(cpus ? *cpus : std::vector<int>{}) {
    threads_.reserve(workers);
    for (unsigned k=1;k<=workers;++k) threads_.emplace_back([this,k]{work(k);});
}

SimPool::~SimPool() {
    {
        std::lock_guard lock(sleepMutex_);
        stop_.store(true,std::memory_order_release);
    }
    sleepCv_.notify_all();
    for (auto& t:threads_) t.join();
}

void SimPool::drain(unsigned slot) {
    for (;;) {
        const size_t k=next_.fetch_add(1,std::memory_order_relaxed);
        if (k>=chunks_) return;
        job_(ctx_,k,slot);
        done_.fetch_add(1,std::memory_order_release);
    }
}

void SimPool::run(size_t chunks,Job job,void* ctx) {
    // gen_ is even here and no worker is inside a job, so the fields are ours.
    job_=job;ctx_=ctx;chunks_=chunks;
    next_.store(0,std::memory_order_relaxed);
    done_.store(0,std::memory_order_relaxed);
    gen_.fetch_add(1,std::memory_order_seq_cst);   // open
    {
        std::lock_guard lock(sleepMutex_);
        if (sleeping_) sleepCv_.notify_all();
    }
    drain(0);
    auto relax=[](unsigned spins) { if (spins<4096) cpuRelax(); else std::this_thread::yield(); };
    // Every chunk finished (the release in each increment publishes its writes).
    for (unsigned spins=0;done_.load(std::memory_order_acquire)<chunks;++spins) relax(spins);
    // Close, then wait out the workers that joined before the close: a worker
    // registers in inflight_ BEFORE it re-checks that the job is still open, so
    // one that saw it open is counted here, and one that comes later sees it
    // closed and never touches the fields (seq_cst on both sides).
    gen_.fetch_add(1,std::memory_order_seq_cst);
    for (unsigned spins=0;inflight_.load(std::memory_order_seq_cst);++spins) relax(spins);
}

void SimPool::open(size_t chunks,Job job,void* ctx) {
    // As run() opens a job, without draining or waiting: finish() closes it.
    job_=job;ctx_=ctx;chunks_=chunks;
    next_.store(0,std::memory_order_relaxed);
    done_.store(0,std::memory_order_relaxed);
    async_=true;
    if (threads_.empty() || !chunks) return;   // the caller runs every chunk (help / finish)
    gen_.fetch_add(1,std::memory_order_seq_cst);   // open
    std::lock_guard lock(sleepMutex_);
    if (sleeping_) sleepCv_.notify_all();
}

bool SimPool::help() {
    if (!async_) return false;
    const size_t k=next_.fetch_add(1,std::memory_order_relaxed);
    if (k>=chunks_) return false;
    job_(ctx_,k,0);
    done_.fetch_add(1,std::memory_order_release);
    return true;
}

void SimPool::finish() {
    if (!async_) return;
    drain(0);
    auto relax=[](unsigned spins) { if (spins<4096) cpuRelax(); else std::this_thread::yield(); };
    for (unsigned spins=0;done_.load(std::memory_order_acquire)<chunks_;++spins) relax(spins);
    if (!threads_.empty() && chunks_) {
        gen_.fetch_add(1,std::memory_order_seq_cst);   // close, then wait out the workers inside (see run)
        for (unsigned spins=0;inflight_.load(std::memory_order_seq_cst);++spins) relax(spins);
    }
    async_=false;
}

void SimPool::work(unsigned slot) {
    // A new thread starts on its creator's CPUs, which is one CPU when that is a
    // pinned simulation thread: take this slot's own, or all of the process's.
    if (pin_) placeThisThread(slot<cpus_.size() ? cpus_[slot] : -1);
    uint64_t seen=0;
    for (;;) {
        bool ready=false;
        const auto until=std::chrono::steady_clock::now()+kSpin;
        for (unsigned spins=0;;++spins) {
            if (gen_.load(std::memory_order_acquire)!=seen) { ready=true;break; }
            if (stop_.load(std::memory_order_acquire)) return;
            if ((spins&63)==63 && std::chrono::steady_clock::now()>=until) break;
            cpuRelax();
        }
        if (!ready) {
            std::unique_lock lock(sleepMutex_);
            ++sleeping_;
            sleepCv_.wait(lock,[&]{
                return stop_.load(std::memory_order_acquire) || gen_.load(std::memory_order_acquire)!=seen;
            });
            --sleeping_;
            if (stop_.load(std::memory_order_acquire)) return;
        }
        const uint64_t g=gen_.load(std::memory_order_acquire);
        seen=g;
        if (!(g&1)) continue;   // the job closed before this worker got here
        inflight_.fetch_add(1,std::memory_order_seq_cst);
        if (gen_.load(std::memory_order_seq_cst)==g) drain(slot);
        inflight_.fetch_sub(1,std::memory_order_seq_cst);
    }
}

} // namespace tak::sim
