#include "sim/retailtrace.h"
#include <iostream>

using namespace tak::sim;

namespace {
struct Digest {
    uint64_t value=14695981039346656037ull;
    void add(uint64_t word) {
        for (int i=0;i<8;++i) { value^=uint8_t(word>>(i*8)); value*=1099511628211ull; }
    }
    void point(RetailReachability::Point p) { add(uint32_t(p.x));add(uint32_t(p.z)); }
    void attempt(const RetailSearchAttempt& a) {
        for (int v:{a.phase,a.partialDistance,a.nodeLimit,a.spread,a.initialDistance,
                    a.heading,a.retry,a.weight}) add(uint32_t(v));
        const auto& t=a.trace;
        for (int v:{t.width,t.height,t.phase,t.best,t.visited,t.visitLimit,t.slopes,t.ground,
                    t.roads,t.dirA,t.dirB,t.nearTraffic,t.otherTraffic,t.started,t.work,t.trafficRadius})
            add(uint32_t(v));
        for (auto p:{t.start,t.goal,t.directEnd,t.march,t.origin,t.a,t.b}) point(p);
        for (const auto* cells:{&t.cells,&a.cost.cells}) {
            add(cells->size());
            for (const auto& c:*cells) { add(c.flags);add(c.direction);add(uint32_t(c.node)); }
        }
        add(a.cost.processed);add(a.cost.heuristicWeight);add(uint32_t(a.cost.endpoint));
        add(a.cost.nodes.size());
        for (const auto& n:a.cost.nodes) {
            for (int v:{n.cell,n.cost,n.priority,n.heapIndex,int(n.gradeCost),int(n.straightSteps)})
                add(uint32_t(v));
        }
        add(a.cost.heap.size());for (int v:a.cost.heap) add(uint32_t(v));
        add(a.route.flags);add(a.route.points.size());for (auto p:a.route.points) point(p);
    }
};

// This digest was captured from the pre-optimization, full-plane-reset worker.
// It includes every callback, slice work charge/notification, raw scratch cell,
// heap entry and route across repeated requests sharing one worker allocation.
uint64_t transcript() {
    Digest digest;
    RetailSearchWorker worker;
    // Imported scratch directions survive the very first initialization too.
    worker.attempt.trace.cells.resize(32*24);
    for (size_t i=0;i<worker.attempt.trace.cells.size();++i)
        worker.attempt.trace.cells[i]={uint8_t(i*7),uint8_t(i%8),int(i)};
    for (int request=0;request<96;++request) {
        const int width=request%4==0 ? 48 : request%4==1 ? 16 : 32;
        const int height=request%3==0 ? 24 : 16;
        const int kind=request%8;
        const RetailReachability::Point goal{width-4,height-4};
        const bool cancel=request%11==5;
        bool complete=false,restored=false;
        digest.add(request);
        for (int tick=0;tick<20000 && !complete;++tick) {
            digest.add(tick);
            auto slice=worker.dispatch(tick==0,request%3==0 ? 1 : request%3==1 ? 37 : 12000,
                [&](int retry) {
                    digest.add(1);digest.add(retry);
                    RetailSearchAttempt::Parameters p;
                    p.width=width;p.height=height;p.start=kind==7 ? RetailReachability::Point{-1,2}
                                                                             : RetailReachability::Point{2,2};
                    p.heading=request*997;p.trafficRadius=8;
                    p.weight=request%9==0 ? 1 : 98304+request*16384;
                    p.costs=retailPathCosts(uint16_t(request*19),1,65536,65536,0,request%7==0);
                    return p;
                },[&](bool last) { digest.add(2);digest.add(last); },
                [&] {
                    digest.add(3);
                    if (kind==6) return std::vector<RetailReachability::Point>{};
                    return std::vector<RetailReachability::Point>{goal,goal,{-4,2},{width+1,height}};
                },[&](int x,int z) {
                    digest.add(4);digest.add(uint32_t(x));digest.add(uint32_t(z));
                    return kind==5 || (x==goal.x && z==goal.z);
                },[&](int x,int z) {
                    digest.add(5);digest.add(uint32_t(x));digest.add(uint32_t(z));
                    return retailGoalDistance(x-goal.x,z-goal.z);
                },[&](int x,int z,int direction) {
                    digest.add(6);digest.add(uint32_t(x));digest.add(uint32_t(z));digest.add(direction);
                    if (x<0 || z<0 || x>=width || z>=height) return 0;
                    if (kind==1 || kind==4)
                        return (z==2 && x>=2 && x<=goal.x) || (x==goal.x && z>=2 && z<=goal.z) ? 6 : 0;
                    if (kind==2 && x==width/2) return 0;
                    if (kind==3) return (x+z)%7==0 ? 5 : (x+z)%5==0 ? 4 : 7;
                    return 6;
                },[&] { digest.add(7);return (request*937+tick*317)&65535; });
            digest.add(int(slice.result));digest.add(slice.work);digest.add(slice.notification);
            digest.add(worker.retry);digest.add(worker.initializationPending);
            digest.attempt(worker.attempt);
            complete=slice.result==RetailSearchAttempt::Result::Complete;
            // Restored planes have arbitrary external writes and must return to
            // the full-clear fallback at the next initialization.
            if (!restored && kind==4 && worker.attempt.phase==2 && worker.attempt.cost.processed==0) {
                auto& cost=worker.attempt.cost;
                std::vector<RetailCostSearch::Node> ordered;
                for (int node:cost.heap) ordered.push_back(cost.nodes[size_t(node)]);
                cost.restore(width,height,cost.cells,std::move(ordered),0,cost.heuristicWeight,false);
                restored=true;
            }
            if (cancel && tick==4) break;
            if (tick==19999 && !complete) throw std::runtime_error("search fixture did not finish");
        }
    }
    return digest.value;
}

template<class Attempt>
void scratchChecks() {
    if constexpr (requires(Attempt a) { a.trace.scratch.resetCells; }) {
        Attempt a;
        typename Attempt::Parameters p;p.width=p.height=1024;p.start={8,8};
        auto initialize=[&](bool accept) {
            return a.initialize(p,[](bool) {},[] {
                return std::vector<RetailReachability::Point>{{900,900},{900,900}};
            },[&](int,int) {return accept;},[](int x,int z) {return retailGoalDistance(x-900,z-900);});
        };
        initialize(true);
        const auto* plane=a.trace.cells.data();
        initialize(true);
        if (a.trace.cells.data()!=plane || a.trace.scratch.resetCells!=1)
            throw std::runtime_error("a sparse request reset or reallocated the full search plane");

        // Populate phase 2, then cancel it by initializing a different request.
        p.width=32;p.height=24;p.start={2,2};
        a.initialize(p,[](bool) {},[] {return std::vector<RetailReachability::Point>{{27,19}};},
            [](int,int) {return false;},[](int x,int z) {return retailGoalDistance(x-27,z-19);});
        while (a.phase==1)
            a.step([](int x,int z,int) {
                return (z==2 && x>=2 && x<=27) || (x==27 && z>=2 && z<=19) ? 6 : 0;
            },[](int x,int z) {return retailGoalDistance(x-27,z-19);},37);
        a.step([](int x,int z,int) {
            return (z==2 && x>=2 && x<=27) || (x==27 && z>=2 && z<=19) ? 6 : 0;
        },[](int x,int z) {return retailGoalDistance(x-27,z-19);},37);
        const auto nodeCapacity=a.cost.nodes.capacity(),heapCapacity=a.cost.heap.capacity();
        if (!nodeCapacity || !heapCapacity) throw std::runtime_error("cost fixture never allocated nodes");
        const auto before=a.cost.cells;
        initialize(true);
        if (a.cost.nodes.capacity()!=nodeCapacity || a.cost.heap.capacity()!=heapCapacity)
            throw std::runtime_error("initialization discarded cost-search allocation buffers");
        for (size_t i=0;i<a.trace.cells.size();++i) {
            const auto& cell=a.trace.cells[i];
            if (cell.flags || cell.node!=-1 || cell.direction!=before[i].direction)
                throw std::runtime_error("cancelled search left stale flags/nodes or erased directions");
        }
    }
}
}

int main() {
    try {
        const auto digest=transcript();
        std::cout<<"retail search transcript "<<digest<<'\n';
        constexpr uint64_t expected=11705273632564964854ull;
        if (digest!=expected) throw std::runtime_error("retail search behavior changed");
        scratchChecks<RetailSearchAttempt>();
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}
