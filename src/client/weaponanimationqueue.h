#pragma once

#include "sim/retailweapon.h"
#include <deque>

namespace tak {

// Simulation-to-display handoff. The caller owns synchronization; packets wait
// until the render snapshot has reached their tick, and are consumed only once.
class WeaponAnimationQueue {
public:
    struct Shot { uint32_t tick=0, weapons=0; float x=0,z=0; };
private:
    struct Packet { uint32_t tick; int unit; RetailWeaponAnimations callbacks; Shot shot; };
    std::deque<Packet> packets_;
public:
    void push(uint32_t tick,int unit,const RetailWeaponAnimations& callbacks,
              uint32_t firedWeapons=0,float x=0,float z=0) {
        if (!callbacks.count && !firedWeapons) return;
        // Bound memory if rendering stalls for a long time. Retain recent poses.
        if (packets_.size()>=65536) packets_.pop_front();
        packets_.push_back({tick,unit,callbacks,{tick,firedWeapons,x,z}});
    }
    void clear() {packets_.clear();}
    template<class Consume,class Fire> void drain(uint32_t throughTick,Consume&& consume,Fire&& fire) {
        while (!packets_.empty() && int32_t(throughTick-packets_.front().tick)>=0) {
            const auto& packet=packets_.front();
            if (packet.shot.weapons) fire(packet.unit,packet.shot);
            for(unsigned i=0;i<packet.callbacks.count;++i)
                consume(packet.unit,packet.callbacks.at(i));
            packets_.pop_front();
        }
    }
};

} // namespace tak
