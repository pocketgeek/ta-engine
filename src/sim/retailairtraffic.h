#pragma once

#include "sim/retailairattack.h"
#include "sim/retailairtoair.h"
#include "sim/retailflight.h"
#include "sim/retailmission.h"
#include <array>
#include <bit>
#include <cstdint>

namespace ta::sim {

// Kingdoms' air traffic: a per-flyer counter (unit +0x12a, 51dc4b..51dce4)
// stepped from the secondary occupancy lists, read by every flyer mission at
// its wake to push a step-out ahead of itself. TotalA.exe has neither (no
// VTOL_StepOut row in its air mission table 0x4fca18, no mission reading a
// counter): the counter and the missions' check are gone. What is left here
// is the step-out mission itself, which nothing in TA pushes.
// The three step-out missions share handler 4214b0 and differ only in their
// registry flags (+0x5a): VTOL_StepOut (row 5ebee0, 0x1000000, the moving
// group member), VTOL_StepOutStandby (5ebef9, 0x5000000, also counted with
// the group's whole membership) and VTOL_StepOutNoFormation (5ebf12, 0).
// Standby, VTOL_Move_FormationStandby and VTOL_Patrol's standby pushes the
// second; a flying builder at work the third; every other mission the first.
enum class RetailStepOut : uint8_t { None=0, StepOut=1, Standby=2, NoFormation=3 };
constexpr uint32_t retailStepOutFlags(RetailStepOut kind) {
    return kind==RetailStepOut::StepOut ? 0x1000000u : kind==RetailStepOut::Standby ? 0x5000000u : 0u;
}

// 4214b0 stage 0: score the eight octants round the flyer's heading (ahead
// +100, 45 degrees off +0, abeam +100, 135 degrees +200, behind +300, each
// plus rand(80)), add 100 to every octant a live listed overlapping flyer
// lies ahead of (41d780), take the first lowest, and go D - rand(D/4) +
// rand(D/4) px out at a random angle within that octant: a point controller
// (4e40e0, flags 0x30) of radius (rand(5) + 5) x 16.
template<class Others,class Random>
RetailFlightGoal retailStepOutGoal(RetailFlightVector position,uint16_t heading,int32_t distance,
        const Others& others,Random random) {
    static constexpr std::array<int,8> base{100,0,100,200,300,200,100,0};
    std::array<int,8> score{};
    for (int n=0;n<8;++n) score[size_t(n)]=int(random(80))+base[size_t(n)];
    for (const RetailFlightVector& other:others)
        for (int n=0;n<8;++n) if (retailAirAhead(other,position,heading,n)) score[size_t(n)]+=100;
    int best=0;
    for (int n=1;n<8;++n) if (score[size_t(n)]<score[size_t(best)]) best=n;
    const int32_t quarter=distance/4;
    const int32_t inward=int32_t(random(quarter)),outward=int32_t(random(quarter));
    const int32_t length=std::bit_cast<int32_t>(uint32_t(distance+outward-inward)<<16);
    const uint16_t angle=uint16_t(int32_t(random(0x2000))+best*0x2000+heading-0x1000);
    RetailFlightGoal goal;
    goal.point={satSub32(position.x,retailScaledSine(angle,length)),position.y,
                satSub32(position.z,retailScaledCosine(angle,length))};
    goal.flags=0x30;
    goal.radius=int16_t((int(random(5))+5)*16);
    return goal;
}

// 4214b0, the handler of all three step-outs:
//   off the map: 800 px toward 192 px inside the playable edge, radius 128
//             (as AirToGround's stage 7), and the mission ends (5) with that
//             controller still on the navigator;
//   stage 0:  no mover or not a flyer ends the list (7); otherwise the point
//             above (mission +0x22), waiting on the controller (0x700);
//   stage 1:  woken by the controller (0x100 arrival, 0x200 failure, 0x400
//             release): the step-out ends (5).
// D is mission +0x4e, the distance the mission that pushed it chose. Every
// draw is 535cc0, the shared game random number generator.
struct RetailStepOutInput {
    RetailFlightVector position;   // unit +0x68
    uint16_t heading=0;            // unit +0x7e
    int32_t distance=0;            // mission +0x4e
    bool flies=true;               // a mover (unit +8) and canfly (type +0x260 0x800)
    bool onMap=true;               // unit +0xa4 is a sector, not the off-map cell (game +0x19f30)
    int32_t width=0,height=0;      // game +0x19e90 / +0x19e94: the playable map, in pixels
};

// Host: random(bound); install(goal) (4d4d40, which also clears the
// mission's pending controller events 0x3700).
template<class Others,class Host>
int retailVtolStepOut(RetailMissionState& m,RetailFlightVector& point,const RetailStepOutInput& in,
                      const Others& others,Host& host) {
    if (!in.onMap) {                                                    // 4214c8
        host.install(RetailFlightGoal{retailOffMapReturn(in.position,in.width,in.height),0x30,0,128});
        m.waitMask|=0x700;
        return 5;
    }
    switch (m.stage) {
    case 0: {                                                           // 4215f2
        if (!in.flies) return 7;
        const RetailFlightGoal goal=retailStepOutGoal(in.position,in.heading,in.distance,others,
                                                      [&](int32_t n) { return host.random(n); });
        point=goal.point;
        host.install(goal);
        m.waitMask=0x700;
        return 1;
    }
    case 1:                                                             // 4215e4
        return 5;
    default:
        return 7;
    }
}

} // namespace ta::sim
