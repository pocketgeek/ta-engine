#pragma once

#include <cstdint>
#include <stdexcept>

namespace tak::sim {

// Primary order dispatcher (4d8450), independent of individual order handlers.
// The host owns the queue and controller lifetimes. This is not the campaign
// MissionScript runner and does not implement ground/air/build/combat handlers.
struct RetailMissionState {
    uint8_t stage = 0;
    uint32_t waitMask = 0;
    uint32_t deadline = 0xffffffffu;
    uint32_t pending = 0;
    uint32_t flags = 0;

    // 4d6a10: unsigned tick arithmetic, with no wrap-aware comparison at dispatch.
    void sleep(uint32_t tick, uint32_t delay) {
        waitMask |= 1;
        deadline = tick + delay;
    }
};

// 401bb0: Guard_NoMove's slot release and repeat timer. It picks no
// target: an armed structure calls 401a90 (World::retailGuardNoMoveSlots,
// which hands stale slots back to the sweep) and sleeps 7 + rand(15); an
// unarmed one sleeps 150. Weapon firing is the per-tick sweep and update.
template<class Random, class Release>
int retailGuardNoMove(RetailMissionState& m, uint32_t tick, bool enabled,
                      bool armed, Random random, Release release) {
    if (!enabled) return 8;
    if (armed) release();
    m.sleep(tick, armed ? random(15)+7 : 150);
    m.stage=0;
    return 2;
}

// 40781c..4078c6: Standby after the host's diversion check. The host owns
// navigator/weapon initialization and deciding whether a new order is available.
template<class Random, class Initialize, class TryOrder>
int retailStandby(RetailMissionState& m, uint32_t tick, bool hasMover,
                  Random random, Initialize initialize, TryOrder tryOrder) {
    switch (m.stage) {
    case 0:
        if (!hasMover) return 7;
        initialize();
        m.waitMask |= 0x20;
        m.sleep(tick, 1);
        return 1;
    case 1:
        if (tryOrder()) return 5;
        m.waitMask |= 0x20;
        m.sleep(tick, random(7) + 7);
        return 2;
    default:
        return 7;
    }
}

// 4078e0: Standby_Wander, the default mission of wildlife and villagers.
// Stage 0 (407c3c): no mover ends it (7); 519b10(3), and the post
// (+0x2e/+0x30) is where the unit stands; a tick's sleep. Stage 1 (407962):
// off Hold Position (+0x130 & 0x30000) the target scan (4d8370); a hit that
// becomes an attack (Scan returns true) clears the wait mask and returns 3
// with the stage left at 1. Then the wander itself and a sleep of 2 +
// rand(30) waiting on 0x700 (407c13). Stage 2 (40790f): a tick's sleep, back
// to stage 1. Not ported: the wander movement (stage 1's point controller
// round the post or the unit, 407a24..407c06; stage 2's arrival and
// rand(3)/rand(20)/rand(50) pause handling): World keeps its own stroll
// order, so the moving flag +0x4e and the pause count +0x52 stay 0 here and
// stage 2 is the timer wake alone.
template<class Random,class Initialize,class Scan>
int retailStandbyWander(RetailMissionState& m,uint32_t tick,bool hasMover,bool holdPosition,
                        Random random,Initialize initialize,Scan scan) {
    switch (m.stage) {
    case 0:
        if (!hasMover) return 7;
        initialize();
        m.waitMask|=0x20;
        m.sleep(tick,1);
        return 1;
    case 1:
        if (!holdPosition && scan()) { m.waitMask=0; return 3; }
        m.waitMask|=0x700;
        m.sleep(tick,random(30)+2);
        return 1;
    case 2:
        m.sleep(tick,1);
        m.stage=1;
        return 4;
    default:
        return 7;
    }
}

// 4175c8..417741: ordinary VTOL_Standby after boundary recovery/diversion.
// Landing and target-order installation remain explicit host operations.
template<class Random,class Initialize,class TryOrder,class TryLand>
int retailVtolStandby(RetailMissionState& m,uint32_t tick,bool hasMover,bool canFly,
                      bool airborne,Random random,Initialize initialize,TryOrder tryOrder,TryLand tryLand) {
    switch (m.stage) {
    case 0:
        if (!hasMover || !canFly) return 7;
        initialize(); m.waitMask|=0x20; m.sleep(tick,1); return 1;
    case 1:
        if (tryOrder()) { m.waitMask=0; m.stage=0; return 3; }
        return 1;
    case 2:
        if (canFly && airborne && tryLand()) return 5;
        m.waitMask|=0x20; m.sleep(tick,random(7)+7); m.stage=1; return 2;
    default: return 7;
    }
}

struct RetailGroundPoint { int16_t x=0, z=0; };
struct RetailGroundResponse {
    int32_t mode=0;
    RetailGroundPoint origin, goal;
};

// The executable truncates these integer-coordinate distances after sqrt.
// Use an integer root so the leash boundary does not depend on FP rounding.
inline uint32_t retailGroundDistance(RetailGroundPoint a,RetailGroundPoint b) {
    const int64_t x=int32_t(a.x)-int32_t(b.x), z=int32_t(a.z)-int32_t(b.z);
    uint64_t value=uint64_t(x*x+z*z), root=0, bit=uint64_t(1)<<62;
    while (bit>value) bit>>=2;
    while (bit) {
        if (value>=root+bit) { value-=root+bit; root=(root>>1)+bit; }
        else root>>=1;
        bit>>=2;
    }
    return uint32_t(root);
}

inline bool retailGroundReturnNeeded(RetailGroundPoint position,const RetailGroundResponse& response,uint16_t leash) {
    return retailGroundDistance(position,response.goal)>
           retailGroundDistance(response.origin,response.goal)+uint32_t(leash)/3;
}

// Ordinary Move_Ground after the host's two diversion checks. Response owns
// target selection/retaliation and auxiliary attack installation; Resume owns
// any return order needed after that attack. Neither callback may invalidate m
// until this function returns (the World host defers queue insertion).
// ResetGoal owns controller replacement, including cancellation and clearing
// controller events; the existing navigator route must survive replacement.
template<class Random, class ResetGoal, class Respond, class Resume>
int retailGroundMove(RetailMissionState& m, uint32_t& radius,
                          uint32_t tick, uint32_t events, int16_t footX,
                          bool attachmentBlocked, int32_t responseMode, uint8_t moveOrder,
                          Random random, ResetGoal resetGoal, Respond respond, Resume resume) {
    if ((m.flags & 0x8000000u) && random(10) == 0) m.flags &= ~0x8000000u;
    switch (m.stage) {
    case 0:
        if (attachmentBlocked) return 7;
        resetGoal(radius + 4);
        return 1;
    case 1:
        m.waitMask = 0x2700;
        m.sleep(tick, random(5) + 5);
        return 1;
    case 2:
        if (events & 0x100) return 5;
        if (events & 0x2700) {
            radius += uint32_t(int32_t(footX) * 32);
            return 0;
        }
        if ((responseMode<0 || (responseMode>0 && moveOrder)) && respond()) {
            m.stage=responseMode<0 ? 3 : 0;
            return 4;
        }
        m.stage = 1;
        return 4;
    case 3:
        if (!resume()) return 7;
        m.stage=0;
        return 4;
    default:
        return 7;
    }
}

template<class Random,class ResetGoal>
int retailPlainGroundMove(RetailMissionState& m,uint32_t& radius,uint32_t tick,uint32_t events,
                          int16_t footX,bool attachmentBlocked,Random random,ResetGoal resetGoal) {
    if (m.stage==3) throw std::invalid_argument("plain ground move has no auxiliary return state");
    return retailGroundMove(m,radius,tick,events,footX,attachmentBlocked,0,0,
                            random,resetGoal,[]{return false;},[]{return false;});
}

// 402880: Move_Ground_Formation, pushed ahead of a Move_Ground by its two
// group checks (402b00). Level < 0 walks to the moving members' centre with
// a radius-4 circle; level > 0 stops and waits for the group. The mission
// retires as soon as the member is back inside the level's radius. Stage 3's
// target scan (40296c) runs only when the parent carries a response mode;
// Respond owns it and returns true after installing an attack.
template<class OutOfSlot,class Stop,class Centre,class Move,class Respond,class Random>
int retailGroundFormation(RetailMissionState& m,uint32_t tick,int level,bool attached,int32_t responseMode,
                          OutOfSlot outOfSlot,Stop stop,Centre centre,Move move,Respond respond,Random random) {
    if (attached) return 7;
    if (!outOfSlot()) return 5;
    switch (m.stage) {
    case 0:
        if (level>0) stop();
        return 1;
    case 1:
        centre();
        if (level<0) move();
        return 1;
    case 2:
        m.sleep(tick,random(5)+5);
        return 1;
    case 3:
        m.stage=responseMode && respond() ? 0 : 1;
        return 4;
    default:
        return 7;
    }
}

// 417750: VTOL_Move_Formation, pushed ahead of a VTOL_Move by the same two
// group checks as Move_Ground (417e02..418052, structurally 402b00). Unlike
// the ground mission it never stops to wait: both levels fly to the group
// centre. Stage 0 takes the centre (51c700, moving subset for level <= 0)
// as the mission point and begins flight (416c50); stage 1 snaps it to the
// footprint grid, installs a 16 px point controller and sleeps 5..14 ticks,
// returning 0 so the next wake re-takes the centre. The mission retires once
// back inside the level's radius. Level 0 never retires. Back inside, a
// VTOL_Move_FormationStandby (0x4000000; VTOL_Standby and VTOL_LandIfCan
// push it) whose group still moves (moving(), retailGroupMoving) stays
// (41797e..417ad1): attack() (4d8370, then 4d7de0(unit, found, 0, 0, 1, 0):
// true when it put an attack ahead) clears the wait mask and the stage and
// returns 3; else loiter(angle, D) installs a point controller D px off
// along `angle` (radius 64, 4d4d40), the mission waits on 0x700 and returns
// 0. `heading` is the body's (unit +0x7e).
template<class OutOfSlot,class Centre,class Begin,class Move,class Random,class Moving,class Attack,class Loiter>
int retailFlightFormation(RetailMissionState& m,uint32_t tick,int level,bool canFly,uint16_t heading,
                          OutOfSlot outOfSlot,Centre centre,Begin begin,Move move,Random random,
                          Moving moving,Attack attack,Loiter loiter) {
    if (level!=0 && !outOfSlot()) {
        if (!(m.flags&0x4000000u) || !moving()) return 5;
        if (attack()) { m.waitMask=0; m.stage=0; return 3; }
        // Not `far`: MinGW's windows.h defines it away (minwindef.h).
        const uint32_t first=uint32_t(random(20));
        const uint32_t distance=(first+uint32_t(random(10))+10)*16;
        const uint16_t angle=uint16_t(heading-0x800u+uint32_t(random(0x1000)));
        loiter(angle,distance);
        m.waitMask|=0x700;
        return 0;
    }
    switch (m.stage) {
    case 0:
        if (!canFly) return 7;
        centre();begin();
        return 1;
    case 1:
        move();
        m.sleep(tick,random(10)+5);
        return 0;
    default:
        return 7;
    }
}

// 40388d..403b91: Patrol after the host's diversion checks. Initialize owns
// the return waypoint; ResetGoal owns the controller and weapon reset. Action
// checks combat then assistance and returns 0 (none), 3 (inserted), or 8 (failed
// order construction). Queue mutation must be deferred until this returns.
template<class Random,class Initialize,class ResetGoal,class Action>
int retailGroundPatrol(RetailMissionState& m,uint32_t& radius,uint32_t tick,
        uint32_t events,int16_t footX,bool hasMover,Random random,
        Initialize initialize,ResetGoal resetGoal,Action action) {
    if (!hasMover) return 7;
    if ((m.flags&0x8000000u) && random(10)==0) m.flags&=~0x8000000u;
    switch (m.stage) {
    case 0:
        initialize();m.sleep(tick,1);return 1;
    case 1:
        radius=0;return 1;
    case 2:
        resetGoal(radius+4);return 1;
    case 3:
        if (events&0x700u) { m.stage=1;return 6; }
        if (events&0x2000u) {
            radius+=uint32_t(int32_t(footX)*32);
            m.stage=2;m.sleep(tick,1);return 4;
        }
        if (const int result=action()) {
            if (result==3) { m.waitMask=0;m.stage=1; }
            return result;
        }
        m.waitMask=0x2700;m.sleep(tick,random(10)+5);return 2;
    default:return 7;
    }
}

// Host: enabled(), head(), hasNext(state), handle(state, events), random(bound),
// remove(state), rotate(state), clear(), idle(). Re-fetch the head and enabled
// state after every handler: handlers can replace the queue or disable a unit.
template<class Host>
void retailDispatchMissions(uint32_t tick, uint32_t& unitEvents, Host& host) {
    unsigned calls = 0;
    while (host.enabled()) {
        auto* mission = host.head();
        if (!mission) { host.idle(); return; }
        auto& m = *mission;
        if (tick >= m.deadline) {
            m.deadline = 0xffffffffu;
            m.pending |= 1;
        }
        const uint32_t events = (unitEvents | m.pending) & m.waitMask;
        if (m.waitMask && !events) return;
        unitEvents &= ~events;
        m.pending &= ~events;
        m.waitMask = 0;
        // Retail consumes events and clears the mask on the 101st attempt,
        // then clears the queue without invoking another handler.
        if (++calls > 100) { host.clear(); return; }
        switch (host.handle(m, events)) {
        case 0: m.stage = 0; break;
        case 1: ++m.stage; break;
        case 2: case 4: break;
        case 3: m.sleep(tick, host.random(30) + 15); break;
        case 5: case 8: host.remove(m); break;
        case 6: host.rotate(m); break;
        case 9:
            m.flags |= 0x400000;
            if (host.hasNext(m)) host.remove(m);
            else { m.stage = 0; m.sleep(tick, host.random(30) + 15); }
            break;
        default: host.clear(); return;
        }
    }
}

} // namespace tak::sim
