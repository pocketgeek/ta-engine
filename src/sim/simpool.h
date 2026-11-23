#pragma once
// SimPool: a persistent fork-join worker pool for the deterministic simulation.
//
// Work is split into FIXED-size chunks of a range (kUnitChunk units by default),
// never into "count / threads" slices, so the chunk boundaries -- and therefore
// any per-chunk result -- do not depend on how many threads run them. The
// calling thread takes part; workers claim chunks from an atomic counter.
//
// Exactness rules for a caller (see WE-parallel P0):
//   - a chunk writes only its own slots (per-unit records, or arenas indexed by
//     the chunk number), or performs an order-independent update (an atomic OR);
//   - any merge of per-chunk results runs after forChunks returns, in chunk order.
// With 0 workers (World::setSerialThreads, or too small a range) the identical
// chunks run inline in ascending order, so serial == workers by construction.
//
// Workers spin briefly for the next job, then sleep on a condition variable, so
// an idle pool costs nothing between ticks and a fork-join costs no thread spawn.
// The caller never waits for a worker that is asleep or descheduled: it waits
// only for the chunks to finish and for workers already inside the job to
// leave, so a busy machine (two sims in single player) degrades to the caller
// doing the work itself rather than to a stall.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace tak::sim {

class SimPool {
public:
    static constexpr size_t kUnitChunk=256;
    // The widest simulation: the calling thread plus kMaxWorkers workers. Nothing
    // in the pool or its users is sized by a constant (per-slot scratch is sized
    // from workers(), see forChunks); 64 is the widest the determinism tests run
    // (tools/simpool_test.cpp, simpool_passes_test.cpp), so nothing wider is offered.
    static constexpr unsigned kMaxThreads=64,kMaxWorkers=kMaxThreads-1;
    // The CPU threads this process may use, at least 1: its affinity mask on
    // Linux (taskset, containers), the machine's thread count elsewhere.
    static unsigned usableCpus();
    // How many threads a simulation gets, and which CPUs they are pinned to.
    // Scheduling only; pure, so tools/simpool_test.cpp pins every rule.
    //
    // The CPUs the counts are measured against (sim/simcpus.h detects them): P
    // performance cores, E efficiency cores and H further hyperthreads on those
    // cores. A CPU with no performance / efficiency split has P = its physical
    // cores and E = 0. N = threads() is every logical CPU the process may use.
    // (H is `smt`: <windows.h> defines `hyper`.)
    struct Cpus {
        unsigned perf=1,eff=0,smt=0;
        constexpr unsigned threads() const { return perf+eff+smt; }
    };
    // AUTO: one thread for each performance core (each core, on a CPU without the
    // split). The rule lives here and nowhere else.
    static constexpr unsigned autoThreads(const Cpus& cpus) { return std::max(cpus.perf,1u); }
    // The game: a SIMULATION THREADS choice (Options > GAME, Settings::simThreads)
    // is 0 = AUTO, or a count from 1 up to twice the CPU threads (above them is
    // more threads than the computer has). kMaxChoice is the most any machine
    // offers: two simulations of kMaxThreads.
    static constexpr unsigned kMaxChoice=2*kMaxThreads;
    static constexpr unsigned maxChoice(unsigned cpus) { return 2*std::min(std::max(cpus,1u),kMaxThreads); }
    // T, the threads a choice configures on a machine: a count above
    // maxChoice (a profile from a bigger machine) is used as that.
    static constexpr unsigned configured(int choice,const Cpus& cpus) {
        return std::min(choice<=0 ? autoThreads(cpus) : unsigned(choice),maxChoice(cpus.threads()));
    }
    // Which CPUs a count of threads reaches, in the order they are handed out
    // (CpuTopology::order): Options colours the slider by it, blue, green,
    // yellow and red.
    enum class Band { Performance,Efficiency,Hyperthread,Over };
    static constexpr Band band(unsigned threads,const Cpus& cpus) {
        return threads<=cpus.perf ? Band::Performance : threads<=cpus.perf+cpus.eff ? Band::Efficiency
             : threads<=cpus.threads() ? Band::Hyperthread : Band::Over;
    }
    // What a game session's simulations get of T. A game with its own local server
    // (single player, a LOCAL SERVER the player hosts, the Benchmark, --play-map)
    // shares T with it: ceil(T / 2) for the game's simulation and floor(T / 2) for
    // the server's (passed as takserver --sim-threads), each at least 1. Any other
    // session (a game on someone else's server, a replay) has the one simulation,
    // which gets T; `server` is then 0. No simulation gets more than kMaxThreads.
    struct Session { unsigned total,game,server; };
    static constexpr Session sessionThreads(int choice,const Cpus& cpus,bool localServer) {
        const unsigned t=configured(choice,cpus);
        if (!localServer) return {t,std::min(t,kMaxThreads),0};
        return {t,std::min(std::max((t+1)/2,1u),kMaxThreads),std::min(std::max(t/2,1u),kMaxThreads)};
    }
    // sessionThreads as the game applies it: a debug build's TAK_SIM_WORKERS
    // decides AUTO, and only AUTO (debugWorkers; *debugAuto says it did): each
    // simulation then runs that many workers, and `total` is their sum.
    static Session session(int choice,const Cpus& cpus,bool localServer,bool* debugAuto=nullptr);
    // The CPUs a session's simulation threads are pinned to, by slot (a
    // simulation's own thread, then its pool workers): the first min(T, N)
    // entries of `order` (CpuTopology::order), dealt alternately to the game and
    // its local server, game first, so each gets as equal a share of every kind
    // of CPU as there is; with no local server the game takes them all. A thread
    // past the end of its list is not pinned. `pin` false (PIN SIMULATION THREADS
    // off, a system that cannot pin): nothing is.
    struct HandOut { std::vector<int> game,server; };
    static HandOut handOut(const std::vector<int>& order,const Session& session,bool localServer,bool pin=true);
    // takserver: each running game's share of the server's total S (--sim-threads;
    // auto is usableCpus()): floor(S / running games), but never fewer than 2 --
    // than 1 when S is 1, so whoever asked for one thread gets one -- and never
    // more than kMaxThreads. The floor means a busy server can run more simulation
    // threads than it has CPUs.
    static constexpr unsigned serverShare(unsigned total,size_t running) {
        total=std::max(total,1u);
        const unsigned share=unsigned(total/std::max<size_t>(running,1));
        return std::min(std::max(share,std::min(2u,total)),kMaxThreads);
    }
    // Workers per sim when nothing chooses a width: clamp(usableCpus() / 8, 0, 3).
    // The library default, for the tools and tests that build a World and set no
    // width; the game and takserver always set one (World::setSimWorkers, from the
    // rules above). Debug builds honour TAK_SIM_WORKERS here (debugWorkers). A
    // World asks once and keeps the answer (World::simPool): this reads the CPU
    // mask anew on every call.
    static unsigned defaultWorkers();
    // Debug builds: TAK_SIM_WORKERS (0..kMaxWorkers workers beside the ticking
    // thread), else -1; always -1 in a release build. It overrides AUTO, and only
    // AUTO, wherever a width is decided: here, the game's latch and takserver.
    static int debugWorkers();

    // cpus: pin worker k (slot k) to (*cpus)[k], a worker past the end to none
    // (sim::placeThisThread); slot 0 is the caller's, which its owner places.
    // nullptr: no thread's CPUs are touched.
    explicit SimPool(unsigned workers,const std::vector<int>* cpus=nullptr);
    ~SimPool();
    SimPool(const SimPool&)=delete;
    SimPool& operator=(const SimPool&)=delete;

    unsigned workers() const { return unsigned(threads_.size()); }
    // Whether this pool's workers are placed as `cpus` asks (the constructor's).
    bool placedAs(const std::vector<int>* cpus) const { return cpus ? pin_ && cpus_==*cpus : !pin_; }

    // Calls fn(chunkIndex, begin, end, slot) once for every chunk
    // [k*chunk, min(count,(k+1)*chunk)) of [0,count). slot identifies the
    // executing thread (0 = the caller, 1..workers()), for per-thread scratch.
    // A range shorter than minParallel, or one chunk, runs inline on the caller.
    // fn must not throw.
    template<class Fn>
    void forChunks(size_t count,size_t chunk,size_t minParallel,Fn&& fn) {
        if (!count) return;
        if (!chunk) chunk=kUnitChunk;
        const size_t chunks=(count+chunk-1)/chunk;
        if (threads_.empty() || chunks<2 || count<minParallel || async_) {
            for (size_t k=0;k<chunks;++k) fn(k,k*chunk,std::min(count,(k+1)*chunk),0u);
            return;
        }
        struct Ctx { Fn* fn;size_t count,chunk; };
        Ctx ctx{&fn,count,chunk};
        run(chunks,[](void* p,size_t k,unsigned slot) {
            auto& c=*static_cast<Ctx*>(p);
            (*c.fn)(k,k*c.chunk,std::min(c.count,(k+1)*c.chunk),slot);
        },&ctx);
    }

    // Asynchronous jobs (Legion 10,000 stage 9): start() opens a job of `chunks` chunks that the workers begin at
    // once and returns without waiting; the caller goes on with its own work, may run unclaimed chunks itself
    // (help(), e.g. while it waits for one chunk's result), and must call finish() -- which runs what is left and
    // waits for every chunk and every worker to leave -- before any other job on this pool. While one is open,
    // forChunks runs inline (busy()). fn(chunkIndex, slot) must stay valid until finish() returns, and each chunk
    // must write only its own slots. With no workers nothing runs until help()/finish(), on the caller.
    template<class Fn>
    void start(size_t chunks,Fn& fn) {
        open(chunks,[](void* p,size_t k,unsigned slot) {(*static_cast<Fn*>(p))(k,slot);},&fn);
    }
    bool help();
    void finish();
    bool busy() const { return async_; }

private:
    using Job=void(*)(void*,size_t,unsigned);
    void open(size_t chunks,Job job,void* ctx);
    bool async_=false;
    void run(size_t chunks,Job job,void* ctx);
    void work(unsigned slot);
    void drain(unsigned slot);

    const bool pin_;
    const std::vector<int> cpus_;       // by slot, when pin_
    std::vector<std::thread> threads_;
    // The current job; written by the caller while gen_ is even (closed), read
    // by a worker only after it has registered in inflight_ and seen gen_ odd.
    Job job_=nullptr;
    void* ctx_=nullptr;
    size_t chunks_=0;
    std::atomic<size_t> next_{0};       // next unclaimed chunk
    std::atomic<size_t> done_{0};       // chunks finished
    std::atomic<unsigned> inflight_{0}; // workers inside the current job
    std::atomic<uint64_t> gen_{0};      // odd: a job is open; even: closed
    std::atomic<bool> stop_{false};
    std::mutex sleepMutex_;
    std::condition_variable sleepCv_;
    unsigned sleeping_=0;               // under sleepMutex_
};

} // namespace tak::sim
