#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>

namespace tak {

enum class RetailFlightAnimationCall {
    BeginFlight,
    BeginLanding,
    EndTransport,
};

struct RetailFlightAnimationState {
    uint32_t beginFlightSerial = 0;
    uint32_t landingSerial = 0;
    bool airborne = false;
    bool beginFlightCallbackPending = false;
    bool landingCallbackPending = false;
};

// Retail queues BeginFlight when a flight mission starts and queues landing
// callbacks when a landing site is accepted, before the final descent runs.
// Mode transitions are fallbacks only when their native call-ins were absent.
template<class Start>
void updateRetailFlightAnimation(RetailFlightAnimationState& state,
        bool airborne,uint32_t beginFlightSerial,uint32_t landingSerial,
        bool canTransport,Start&& start) {
    const uint32_t beginFlightEvents = beginFlightSerial - state.beginFlightSerial;
    const bool landingEvent = landingSerial != state.landingSerial;
    if (airborne != state.airborne) {
        state.airborne = airborne;
        if (airborne) {
            state.landingCallbackPending = false;
            if (!beginFlightEvents && !state.beginFlightCallbackPending)
                start(RetailFlightAnimationCall::BeginFlight);
            state.beginFlightCallbackPending = false;
        } else {
            if (!landingEvent && !state.landingCallbackPending)
                start(RetailFlightAnimationCall::BeginLanding);
            state.beginFlightCallbackPending = false;
            state.landingCallbackPending = false;
        }
    }
    if (beginFlightEvents) {
        for (uint32_t i=0;i<beginFlightEvents;++i)
            start(RetailFlightAnimationCall::BeginFlight);
        state.beginFlightSerial = beginFlightSerial;
        state.beginFlightCallbackPending = !airborne;
    }
    if (landingEvent) {
        if (canTransport) start(RetailFlightAnimationCall::EndTransport);
        start(RetailFlightAnimationCall::BeginLanding);
        state.landingSerial = landingSerial;
        state.beginFlightCallbackPending = false;
        state.landingCallbackPending = airborne;
    }
}

// Preserve the ordering of transitions on different simulation ticks when the
// renderer skips snapshots. Callers synchronize capture/drain across threads.
class RetailFlightAnimationQueue {
public:
    struct Snapshot {
        uint32_t tick;
        int unit;
        bool airborne;
        uint32_t beginFlightSerial,landingSerial;
    };
private:
    std::unordered_map<int,Snapshot> latest_;
    std::deque<Snapshot> pending_;
public:
    // Whether capture() would record this state. It reads only latest_, which capture()
    // alone writes, so it may run concurrently with drain() (pending_ only) and with
    // other changed() calls, as long as no capture() runs at the same time.
    bool changed(int unit,bool airborne,uint32_t begin,uint32_t landing) const {
        const auto previous=latest_.find(unit);
        return previous==latest_.end() || previous->second.airborne!=airborne ||
               previous->second.beginFlightSerial!=begin || previous->second.landingSerial!=landing;
    }
    void capture(uint32_t tick,int unit,bool airborne,uint32_t begin,uint32_t landing) {
        if(!changed(unit,airborne,begin,landing))return;
        Snapshot snapshot{tick,unit,airborne,begin,landing};
        latest_.insert_or_assign(unit,snapshot);
        if(pending_.size()>=65536)pending_.pop_front();
        pending_.push_back(snapshot);
    }
    void clear() {latest_.clear();pending_.clear();}   // the states restart (a replay jump)
    template<class Consume> void drain(uint32_t throughTick,Consume&& consume) {
        while(!pending_.empty() && int32_t(throughTick-pending_.front().tick)>=0) {
            consume(pending_.front());
            pending_.pop_front();
        }
    }
};

} // namespace tak
