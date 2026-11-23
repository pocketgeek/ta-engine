#pragma once
#include "sim/navigationtelemetry.h"
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <vector>

namespace crowdbench_matrix {
// Diagnostic-only allocation overhead is deliberately excluded from normal
// timing runs. tick is set before issuing fixture commands and World::tick.
// When `quiet` names an allocation-counting switch, the observer's own
// bookkeeping allocations are not counted as simulation allocations (serial
// execution only: a paused switch would also hide concurrent worker allocations).
struct LatencyObserver final:tak::sim::NavigationTelemetry {
    using Clock=std::chrono::steady_clock;
    std::atomic<bool>* quiet=nullptr;
    struct Quiet {
        std::atomic<bool>* flag;bool previous=false;
        explicit Quiet(std::atomic<bool>* f):flag(f) {if(flag)previous=flag->exchange(false,std::memory_order_relaxed);}
        ~Quiet() {if(flag)flag->store(previous,std::memory_order_relaxed);}
    };
    struct Pending {uint64_t tick;int unit;Clock::time_point requested;};
    uint64_t tick=0,sequence=0,empty=0,unknown=0;
    std::array<uint64_t,4> cancellations{};
    std::map<uint64_t,Pending> pending;
    std::vector<double> deliveries,successful,failed,deliveryWallMs;
    uint64_t requested(int unit) override {
        const Quiet pause(quiet);
        pending.emplace(++sequence,Pending{tick,unit,Clock::now()});return sequence;
    }
    void cancelled(uint64_t token,Cancel reason) override {
        if(!token)return;
        const Quiet pause(quiet);
        if(pending.erase(token))++cancellations[size_t(reason)];else ++unknown;
    }
    void delivered(uint64_t token,bool failure,size_t points) override {
        if(!token)return;
        const Quiet pause(quiet);
        const auto it=pending.find(token);
        if(it==pending.end()) {++unknown;return;}
        const double age=double(tick-it->second.tick);
        deliveryWallMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-it->second.requested).count());
        deliveries.push_back(age);(failure?failed:successful).push_back(age);
        empty+=points==0;pending.erase(it);
    }
    std::vector<double> pendingAges() const {
        std::vector<double> ages;ages.reserve(pending.size());
        for(const auto& [token,request]:pending) {(void)token;ages.push_back(double(tick-request.tick));}
        return ages;
    }
};
}
