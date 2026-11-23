// SimPool: every chunk runs exactly once with the fixed boundaries at any
// width, per-chunk results merged in chunk order equal the serial run, and the
// pool survives many back-to-back jobs (the per-tick pattern) at every width a
// player or a server operator can choose (SIMULATION THREADS, --sim-threads): 0
// workers, each count the default rule gives (1, 2, 3), a 24-thread machine's
// most (23) and the code's most (SimPool::kMaxWorkers, 63), with 5, 7, 15 and
// 31 between; asynchronous jobs (start/help/finish) run every chunk exactly
// once. At each width every slot of the pool runs, one thread to a slot (what
// per-slot scratch relies on). And the rules that decide a simulation's width
// and its CPUs: the game's SIMULATION THREADS (AUTO, the split with a local
// server), takserver's share per running game, the slider's colour bands, the
// Linux CPU topology reader on fixture trees, and who is pinned to which CPU.
#include "sim/simcpus.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs=std::filesystem;
using tak::sim::SimPool;

static int failures=0;
static void check(bool ok,const char* what,unsigned workers) {
    if (!ok) { std::fprintf(stderr,"FAIL (workers=%u): %s\n",workers,what);++failures; }
}

static uint64_t mix(uint64_t h,uint64_t v) { return (h^v)*0x100000001b3ull; }

// The Linux topology reader (sim::readLinuxCpus) on small sysfs trees written here, the
// order threads take the CPUs in and the counts for each; then who gets which CPU
// (SimPool::handOut) for a game with a local server and one without, below, at and
// above the computer's threads.
struct FakeCpu { int id;std::string siblings;int package,core,capacity; };   // siblings "": no such file; capacity 0: none
static fs::path fakeSysfs(const char* name,const std::vector<FakeCpu>& cpus,const char* perf,const char* eff) {
    const fs::path root=fs::temp_directory_path()/("tak-simpool-test-"+std::to_string(std::random_device{}()))/name;
    auto put=[](const fs::path& file,const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream(file)<<text<<"\n";
    };
    for (const FakeCpu& c:cpus) {
        const fs::path cpu=root/"devices"/"system"/"cpu"/("cpu"+std::to_string(c.id));
        if (!c.siblings.empty()) put(cpu/"topology"/"thread_siblings_list",c.siblings);
        if (c.core>=0) { put(cpu/"topology"/"core_id",std::to_string(c.core));put(cpu/"topology"/"physical_package_id",std::to_string(c.package)); }
        if (c.capacity) put(cpu/"cpu_capacity",std::to_string(c.capacity));
    }
    if (perf) put(root/"devices"/"cpu_core"/"cpus",perf);
    if (eff) put(root/"devices"/"cpu_atom"/"cpus",eff);
    return root;
}
static bool is(const tak::sim::CpuTopology& t,unsigned perf,unsigned eff,unsigned smt,const char* order) {
    return t.perf==perf && t.eff==eff && t.smt==smt && t.canPin && tak::sim::cpuListText(t.order)==order;
}
static void checkCpus() {
    using namespace tak::sim;
    std::vector<fs::path> roots;
    // 8 performance + 16 efficiency cores, no hyperthreading (Intel's hybrid lists).
    std::vector<FakeCpu> cpus;
    for (int i=0;i<24;++i) cpus.push_back({i,std::to_string(i),0,i,0});
    roots.push_back(fakeSysfs("hybrid",cpus,"0-7","8-23"));
    check(is(readLinuxCpus(roots.back(),{}),8,16,0,"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23"),
          "8 P + 16 E, no hyperthreading",24);
    std::vector<int> some;
    for (int i=4;i<24;++i) some.push_back(i);
    check(is(readLinuxCpus(roots.back(),some),4,16,0,"4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23"),
          "only the CPUs the process may use (taskset -c 4-23)",20);
    check(is(readLinuxCpus(roots.back(),{8,9,10,11}),4,0,0,"8,9,10,11"),"efficiency cores alone are plain cores",4);
    // 4 cores, 8 threads, read from package and core ids (no sibling lists).
    cpus.clear();
    for (int i=0;i<8;++i) cpus.push_back({i,"",0,i/2,0});
    roots.push_back(fakeSysfs("smt",cpus,nullptr,nullptr));
    check(is(readLinuxCpus(roots.back(),{}),4,0,4,"0,2,4,6,1,3,5,7"),"4 cores with hyperthreading",8);
    check(is(readLinuxCpus(roots.back(),{0,1,2}),2,0,1,"0,2,1"),"two siblings and one more core",3);
    // 2 hyperthreaded performance cores + 4 efficiency cores.
    cpus={{0,"0-1",0,0,0},{1,"0-1",0,0,0},{2,"2-3",0,1,0},{3,"2-3",0,1,0},{4,"4",0,2,0},{5,"5",0,3,0},{6,"6",0,4,0},{7,"7",0,5,0}};
    roots.push_back(fakeSysfs("hybrid-smt",cpus,"0-3","4-7"));
    const CpuTopology mixed=readLinuxCpus(roots.back(),{});
    check(is(mixed,2,4,2,"0,2,4,5,6,7,1,3"),"hybrid with hyperthreading: cores, then efficiency cores, then siblings",8);
    // No hybrid lists: the highest cpu_capacity is a performance core. Nothing readable: plain cores.
    cpus={{0,"0",0,0,512},{1,"1",0,1,512},{2,"2",0,2,1024},{3,"3",0,3,1024}};
    roots.push_back(fakeSysfs("capacity",cpus,nullptr,nullptr));
    check(is(readLinuxCpus(roots.back(),{}),2,2,0,"2,3,0,1"),"cpu_capacity: the big cores first",4);
    check(is(readLinuxCpus(roots.back()/"nothing-here",{3,5,9}),3,0,0,"3,5,9"),"unreadable sysfs: every usable CPU is a core",3);
    check(mixed.known && readLinuxCpus(roots.back(),{}).known && !readLinuxCpus(roots.back()/"nothing-here",{3,5,9}).known,
          "a tree that was read is known; the fallback of plain cores is not",0);
    // CCDs (sim::readLinuxCcds): a 7950X3D's two dies, 8 hyperthreaded cores each, the
    // first with 96 MB of L3 (3D V-Cache), the second 32 MB; and a CPU with one L3.
    {
        cpus.clear();
        for (int i=0;i<32;++i) {
            const int core=i%16;
            cpus.push_back({i,std::to_string(core)+","+std::to_string(core+16),0,core,0});
        }
        roots.push_back(fakeSysfs("x3d",cpus,nullptr,nullptr));
        auto put=[](const fs::path& file,const std::string& text) {
            fs::create_directories(file.parent_path());
            std::ofstream(file)<<text<<"\n";
        };
        for (int i=0;i<32;++i) {
            const fs::path cpu=roots.back()/"devices"/"system"/"cpu"/("cpu"+std::to_string(i));
            const bool first=i%16<8;
            const char* levels[][3]={{"1","Data","32K"},{"1","Instruction","32K"},{"2","Unified","1024K"},
                                     {"3","Unified",first ? "98304K" : "32768K"}};
            for (int k=0;k<4;++k) {
                const fs::path index=cpu/"cache"/("index"+std::to_string(k));
                put(index/"level",levels[k][0]);put(index/"type",levels[k][1]);put(index/"size",levels[k][2]);
                if (k==3) put(index/"shared_cpu_list",first ? "0-7,16-23" : "8-15,24-31");
            }
            put(cpu/"cpufreq"/"cpuinfo_max_freq",first ? "5250000" : "5759000");
        }
        const auto ccds=readLinuxCcds(roots.back());
        check(ccds.size()==2 && cpuRangesText(ccds[0].cpus)=="0-7,16-23" && cpuRangesText(ccds[1].cpus)=="8-15,24-31",
              "two CCDs, by the L3 their CPUs share",32);
        check(ccds.size()==2 && ccds[0].perf==8 && ccds[0].eff==0 && ccds[0].threads==16 && ccdThreads(ccds[0])==8 &&
              ccds[0].l1dKB==32 && ccds[0].l1iKB==32 && ccds[0].l2KB==1024 && ccds[0].l3KB==96*1024 &&
              ccds[1].l3KB==32*1024 && ccds[0].maxMHz==5250 && ccds[1].maxMHz==5759,"each CCD's cores, caches and clock",32);
        check(largestL3(ccds)==0,"the 3D V-Cache die has the largest L3",32);
        // One L3 for every CPU (the hybrid tree has no cache files at all): one CCD.
        const auto single=readLinuxCcds(roots[0]);
        check(single.size()==1 && single[0].threads==24 && single[0].perf==8 && single[0].eff==16 && largestL3(single)==0 &&
              largestL3({Ccd{},Ccd{}})==-1,"one L3: one CCD; equal L3s: none is the largest",24);
        check(cpuRangesText({16,1,2,3,7,8})=="1-3,7,8,16" && cpuRangesText({})=="","CPU ranges for the eye",0);
    }
    for (const fs::path& root:roots) { std::error_code ec;fs::remove_all(root.parent_path(),ec); }
    std::vector<int> list;
    check(parseCpuList("1,3,5-7",list) && cpuListText(list)=="1,3,5,6,7" && parseCpuList("4",list) && list==std::vector<int>{4} &&
          !parseCpuList("",list) && !parseCpuList("1,,2",list) && !parseCpuList("3-1",list) && !parseCpuList("a",list) &&
          !parseCpuList("1-",list) && !parseCpuList("-1",list) && !parseCpuList("1, 2",list) && list==std::vector<int>{4},
          "CPU lists: numbers and ranges, in the order given",0);
    // The hand-out on that last hybrid, L = 0,2,4,5,6,7,1,3 (N = 8, AUTO = 2).
    auto hands=[&](int choice,bool local,const char* game,const char* server,bool pin=true) {
        const SimPool::HandOut h=SimPool::handOut(mixed.order,SimPool::sessionThreads(choice,mixed,local),local,pin);
        return cpuListText(h.game)==game && cpuListText(h.server)==server;
    };
    check(hands(0,true,"0","2") && hands(0,false,"0,2",""),"AUTO: a performance core each",8);
    check(hands(5,true,"0,4,6","2,5") && hands(5,false,"0,2,4,5,6",""),"below N: dealt alternately, game first",8);
    check(hands(8,true,"0,4,6,1","2,5,7,3") && hands(8,false,"0,2,4,5,6,7,1,3",""),"at N: every CPU, an equal share of each kind",8);
    check(hands(12,true,"0,4,6,1","2,5,7,3") && hands(12,false,"0,2,4,5,6,7,1,3","") &&
          SimPool::sessionThreads(12,mixed,true).game==6,"above N: the threads beyond the CPUs get none",8);
    check(hands(1,true,"0","") && hands(1,false,"0",""),"one thread: one CPU",8);
    check(hands(8,true,"","",false) && hands(8,false,"","",false),"pinning off: no CPU for anyone",8);
    check(pinnedText("4,6",0)==" pinned to cpus 4,6" && pinnedText("4,6",1)==" pinned to cpus 4,6; 1 thread not pinned" &&
          pinnedText("1,3 / 5,7",3)==" pinned to cpus 1,3 / 5,7; 3 threads not pinned" && pinnedText("",4).empty(),
          "the log's words for the CPUs",0);
}

int main() {
    check(SimPool::usableCpus()>=1,"usable CPUs",0);
    check(SimPool::kMaxWorkers==63 && SimPool::defaultWorkers()<=SimPool::kMaxWorkers,"the widest pool",0);
    check(SimPool::debugWorkers()<0 || unsigned(SimPool::debugWorkers())==SimPool::defaultWorkers(),
          "TAK_SIM_WORKERS is the default when set",0);
    // The game's rule (SimPool::sessionThreads), on a machine of P performance cores, E
    // efficiency cores and H hyperthreads (N threads in all): T is P at AUTO, a count
    // up to 2N as itself, one above as 2N. A game with its own local server takes
    // ceil(T / 2) and gives the server floor(T / 2), each at least 1; any other game
    // takes T. No simulation runs more than kMaxThreads.
    using Cpus=SimPool::Cpus;
    auto session=[](int choice,Cpus cpus,bool local,unsigned total,unsigned game,unsigned server) {
        const SimPool::Session s=SimPool::sessionThreads(choice,cpus,local);
        return s.total==total && s.game==game && s.server==server;
    };
    const Cpus hybrid{8,16,0},plain{8,0,8},one{1,0,0};   // 24, 16 and 1 threads
    check(session(0,hybrid,true,8,4,4) && session(0,hybrid,false,8,8,0) && session(-3,hybrid,true,8,4,4) &&
          SimPool::autoThreads(hybrid)==8,"AUTO is the performance cores",24);
    check(session(0,plain,true,8,4,4) && session(0,plain,false,8,8,0) && SimPool::autoThreads(plain)==8,
          "AUTO without the split is the cores, not their hyperthreads",16);
    check(session(1,hybrid,true,1,1,1) && session(1,hybrid,false,1,1,0),"T = 1: each at least 1",24);
    check(session(2,hybrid,true,2,1,1) && session(2,hybrid,false,2,2,0),"T = 2",24);
    check(session(5,hybrid,true,5,3,2) && session(5,hybrid,false,5,5,0),"T = 5: the game takes the odd one",24);
    check(session(24,hybrid,true,24,12,12) && session(24,hybrid,false,24,24,0),"T = N",24);
    check(session(48,hybrid,true,48,24,24) && session(48,hybrid,false,48,48,0) && SimPool::maxChoice(24)==48,"T = 2N",24);
    check(session(49,hybrid,true,48,24,24) && session(100000,hybrid,false,48,48,0),"above 2N is 2N",24);
    check(session(0,one,true,1,1,1) && session(0,one,false,1,1,0) && session(3,one,true,2,1,1) && SimPool::maxChoice(1)==2,
          "a one-thread machine",1);
    check(session(0,Cpus{5,0,0},true,5,3,2) && session(0,Cpus{5,0,0},false,5,5,0),"AUTO on an odd count",5);
    check(session(128,Cpus{64,0,0},true,128,64,64) && session(128,Cpus{64,0,0},false,128,64,0) &&
          session(0,Cpus{128,0,0},true,128,64,64) && session(0,Cpus{128,0,0},false,128,64,0) &&
          SimPool::maxChoice(128)==SimPool::kMaxChoice && SimPool::kMaxChoice==128,"no simulation above kMaxThreads",64);
    // The slider's colour for a count of threads (SimPool::band): the CPUs it reaches.
    using Band=SimPool::Band;
    check(SimPool::band(1,hybrid)==Band::Performance && SimPool::band(8,hybrid)==Band::Performance &&
          SimPool::band(9,hybrid)==Band::Efficiency && SimPool::band(24,hybrid)==Band::Efficiency &&
          SimPool::band(25,hybrid)==Band::Over && SimPool::band(48,hybrid)==Band::Over,"bands: 8 + 16, no hyperthreads",24);
    check(SimPool::band(8,plain)==Band::Performance && SimPool::band(9,plain)==Band::Hyperthread &&
          SimPool::band(16,plain)==Band::Hyperthread && SimPool::band(17,plain)==Band::Over,"bands: 8 cores, 16 threads",16);
    check(SimPool::band(2,Cpus{2,4,2})==Band::Performance && SimPool::band(3,Cpus{2,4,2})==Band::Efficiency &&
          SimPool::band(6,Cpus{2,4,2})==Band::Efficiency && SimPool::band(7,Cpus{2,4,2})==Band::Hyperthread &&
          SimPool::band(8,Cpus{2,4,2})==Band::Hyperthread && SimPool::band(9,Cpus{2,4,2})==Band::Over,"bands: all four",8);
    checkCpus();
    // takserver's rule (SimPool::serverShare): floor(S / running games), at least 2
    // (1 when S is 1), at most kMaxThreads.
    check(SimPool::serverShare(24,1)==24 && SimPool::serverShare(24,2)==12 && SimPool::serverShare(24,3)==8 &&
          SimPool::serverShare(24,5)==4 && SimPool::serverShare(24,0)==24,"S over the running games, rounded down",24);
    check(SimPool::serverShare(24,12)==2 && SimPool::serverShare(24,13)==2 && SimPool::serverShare(24,1000)==2 &&
          SimPool::serverShare(3,2)==2 && SimPool::serverShare(2,2)==2,"many games: never fewer than 2",24);
    check(SimPool::serverShare(1,1)==1 && SimPool::serverShare(1,2)==1 && SimPool::serverShare(1,1000)==1 &&
          SimPool::serverShare(0,3)==1,"S = 1 gives 1",1);
    check(SimPool::serverShare(128,1)==64 && SimPool::serverShare(128,2)==64 && SimPool::serverShare(128,3)==42,
          "no game above kMaxThreads",128);
    uint64_t reference=0;
    for (unsigned workers:{0u,1u,2u,3u,5u,7u,15u,23u,31u,SimPool::kMaxWorkers}) {
        SimPool pool(workers);
        check(pool.workers()==workers,"worker count",workers);
        for (size_t count:{size_t(0),size_t(1),size_t(255),size_t(256),size_t(257),size_t(5000),size_t(20011)}) {
            const size_t chunks=(count+SimPool::kUnitChunk-1)/SimPool::kUnitChunk;
            std::vector<uint32_t> hits(count,0);
            std::vector<uint64_t> partial(chunks,0);
            std::vector<uint8_t> boundsOk(chunks,0);
            pool.forChunks(count,SimPool::kUnitChunk,0,[&](size_t k,size_t b,size_t e,unsigned slot) {
                boundsOk[k]=b==k*SimPool::kUnitChunk && e==std::min(count,b+SimPool::kUnitChunk) && slot<=workers;
                uint64_t h=0xcbf29ce484222325ull;
                for (size_t i=b;i<e;++i) { ++hits[i];h=mix(h,i*2654435761u); }
                partial[k]=h;
            });
            bool once=true;for (auto n:hits) once&=n==1;
            check(once,"every index exactly once",workers);
            bool bounds=true;for (auto ok:boundsOk) bounds&=ok!=0;
            check(bounds,"fixed chunk boundaries",workers);
            uint64_t merged=0xcbf29ce484222325ull;
            for (auto p:partial) merged=mix(merged,p);   // ordered merge
            if (count==20011) {
                if (workers==0) reference=merged;
                check(merged==reference,"ordered merge equals serial",workers);
            }
        }
        // Many small jobs in a row, as one tick's phases dispatch them.
        std::vector<uint32_t> sum(4096,0);
        for (int rep=0;rep<2000;++rep)
            pool.forChunks(sum.size(),SimPool::kUnitChunk,0,[&](size_t,size_t b,size_t e,unsigned) {
                for (size_t i=b;i<e;++i) ++sum[i];
            });
        bool all=true;for (auto v:sum) all&=v==2000;
        check(all,"repeated jobs",workers);
        // Below minParallel the caller runs every chunk itself, in order.
        std::vector<size_t> order;
        pool.forChunks(1000,SimPool::kUnitChunk,1500,[&](size_t k,size_t,size_t,unsigned slot) {
            order.push_back(k);check(slot==0,"inline below minParallel",workers);
        });
        check(order==std::vector<size_t>{0,1,2,3},"inline chunk order",workers);
        // slot identifies the executing thread: scratch kept per slot (World's
        // aura masks and exploration lists, sized workers()+1) is one thread's
        // at a time. A job of workers+1 chunks, each of which waits until all
        // of them have started, ends only with every thread of the pool inside
        // it at once -- so every slot 0..workers is in use, and none twice.
        for (int rep=0;rep<8 && workers;++rep) {
            const size_t n=size_t(workers)+1;
            std::vector<std::atomic<uint32_t>> inSlot(n);
            std::atomic<size_t> started{0};
            std::atomic<bool> shared{false},stuck{false};
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            pool.forChunks(n,1,0,[&](size_t,size_t,size_t,unsigned slot) {
                if (slot>=n || inSlot[slot].fetch_add(1)!=0) shared=true;
                started.fetch_add(1);
                while (started.load()<n && !stuck.load()) {
                    if (std::chrono::steady_clock::now()>until) stuck=true;
                    std::this_thread::yield();
                }
                if (slot<n) inSlot[slot].fetch_sub(1);
            });
            check(!stuck,"every slot of the pool runs",workers);
            check(!shared,"one thread to a slot",workers);
        }
        // Asynchronous jobs (Legion stage 9): the caller keeps working while the workers run the chunks, may help,
        // waits for one chunk by helping, and finish() leaves every chunk run exactly once; while a job is open
        // forChunks runs inline on the caller; and many start/finish rounds back to back.
        for (int rep=0;rep<500;++rep) {
            const size_t n=size_t(1+rep%37);
            std::vector<std::atomic<uint32_t>> runs(n);
            auto fn=[&](size_t k,unsigned slot) {
                runs[k].fetch_add(1,std::memory_order_relaxed);
                if (slot>workers) check(false,"async slot in range",workers);
            };
            pool.start(n,fn);
            check(pool.busy(),"busy while open",workers);
            if (rep%3==0) {
                std::vector<size_t> inl;
                pool.forChunks(1000,SimPool::kUnitChunk,0,[&](size_t k,size_t,size_t,unsigned slot) {
                    inl.push_back(k);check(slot==0,"forChunks inline while an async job is open",workers);
                });
                check(inl==std::vector<size_t>{0,1,2,3},"inline order while open",workers);
            }
            if (rep%2==0) while (pool.help()) {}
            pool.finish();
            check(!pool.busy(),"not busy after finish",workers);
            bool once=true;for (auto& r:runs) once&=r.load()==1;
            check(once,"async: every chunk exactly once",workers);
        }
    }
    if (failures) return 1;
    std::puts("simpool ok");
    return 0;
}
