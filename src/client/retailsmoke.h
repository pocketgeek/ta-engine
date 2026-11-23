#pragma once

#include <array>
#include <algorithm>
#include <bit>
#include <cstdint>

namespace tak {

// Retail inserts newly burning features at the active-list head, then walks
// that list newest-first. Within each burn it advances old smoke before asking
// for the next emission. The event type is intentionally generic so this exact
// ordering can be shared by the renderer and its deterministic regression.
template<class Events>
void retailOrderFeatureSmokeNewestFirst(Events& events) {
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b) {
        return a.activationSequence>b.activationSequence;
    });
}

template<class Events,class Update,class Emit>
void retailStepFeatureSmoke(const Events& events,Update&& update,Emit&& emit) {
    for(const auto& event:events) {
        update(event);
        if(event.emit)emit(event);
    }
}

// Display-only smoke particle (retail 4f1cf0/4f1bd0). Coordinates are 16.16.
struct RetailSmokeParticle {
    std::array<int32_t,3> position{};
    int32_t period=8;
    uint32_t frameLimit=0, countdown=8, frame=0;
    bool small=false, steam=false;

    template<class Random>
    bool tick(int32_t windX,int32_t windZ,int32_t gravity,Random random) {
        const std::array<uint32_t,3> delta={uint32_t(windX)*8u,
            uint32_t(gravity)*4u,uint32_t(windZ)*8u};
        for(unsigned i=0;i<3;++i)
            position[i]=std::bit_cast<int32_t>(uint32_t(position[i])+delta[i]);
        if(--countdown!=0)return true;
        const int32_t half=period/2;
        countdown=uint32_t(half+int64_t(random())*half/32768);
        return ++frame!=frameLimit;
    }
};

} // namespace tak
