#pragma once

#include "sim/retailmotion.h"
#include "sim/retailmission.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <optional>

namespace ta::sim {

struct RetailFlightVector { int32_t x = 0, y = 0, z = 0; };

// Positions and the points flown to are raw 16.16 words. Retail adds and
// subtracts them in 32-bit registers, where far-apart values wrap; the ports
// here use satAdd32 / satSub32 (fixed.h), which give retail's result wherever
// it is in range and stop at the end of the range where retail would wrap
// (ours: only a map 32768 px wide, 2048 cells, reaches it). A point past the
// east or south edge of such a map is therefore a point ON that edge, which
// the flyer can reach, and a flyer is never steered the short way round a
// wrapped world.
//
// The exception is a destination that is only a direction: VTOL_Evade's line
// (retailAirEvade), whose far end lies outside the range on a map of any size.
// Retail's wrapped arithmetic is what makes it work, so that one destination
// keeps it (RetailFlightNavigation::wraps).
inline int32_t retailWrappedSub(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(uint32_t(a) - uint32_t(b));
}

// 417188..4172ff: candidate selection after the current point failed landing.
// Each draw includes both endpoints; candidates snap to the rectangular body.
// The final orbit is anchored to the mission origin, not the current position.
template<class Random,class Landable>
std::optional<RetailFlightVector> retailLandingCandidates(RetailFlightVector position,
        int16_t footX,int16_t footZ,Random random,Landable landable) {
    auto snap=[](int32_t coordinate,int16_t foot) {
        const uint32_t shifted=uint32_t(coordinate)-uint32_t(int32_t(foot)*524288)+0x80000u;
        const int16_t cell=int16_t(std::bit_cast<int32_t>(shifted)>>20);
        return std::bit_cast<int32_t>(uint32_t(int32_t(foot)+int32_t(cell)*2)<<19);
    };
    for (int half=64;half<256;half+=16) {
        const auto dx=uint32_t(random(half*2+1)-uint32_t(half))<<16;
        const auto dz=uint32_t(random(half*2+1)-uint32_t(half))<<16;
        RetailFlightVector candidate{
            snap(satAdd32(position.x,std::bit_cast<int32_t>(dx)),footX),position.y,
            snap(satAdd32(position.z,std::bit_cast<int32_t>(dz)),footZ)};
        if (landable(candidate)) return candidate;
    }
    return {};
}

template<class Random,class Landable>
RetailFlightVector retailLandingSearch(RetailFlightVector position,RetailFlightVector anchor,
        int16_t footX,int16_t footZ,uint32_t events,uint32_t& angle,bool& orbit,
        Random random,Landable landable) {
    if (auto point=retailLandingCandidates(position,footX,footZ,random,landable)) {
        orbit=false; return *point;
    }
    orbit=true;
    if (events&0x700) angle-=21845;
    anchor.x=satSub32(anchor.x,retailScaledSine(uint16_t(angle),160*65536));
    anchor.z=satSub32(anchor.z,retailScaledCosine(uint16_t(angle),160*65536));
    return anchor;
}

struct RetailFlightGoal {
    RetailFlightVector point;
    uint16_t flags = 0x20, heading = 0;
    int16_t radius = 0;
    bool accepts(RetailFlightVector position) const {
        const double dx = double(position.x) - point.x, dz = double(position.z) - point.z;
        const double distance = std::sqrt(dx*dx + dz*dz) / 65536;
        if (flags & 0x10) return distance < radius;
        return distance <= 0.5 && (!(flags & 8) || std::abs(int64_t(position.y)-point.y) <= 65536);
    }
};

// 41de5d..41df64: hover attack chooses a point on the target-facing ring.
// Preserve the current radius within 16 pixels; otherwise choose distance-3..+4.
// The occasional angular/radial draws are independent, in retail draw order.
template<class Random>
RetailFlightGoal retailHoverAttackGoal(RetailFlightVector position,
        RetailFlightVector target, int distance, Random random) {
    uint16_t angle=uint16_t(retailDirection(Fixed::raw(satSub32(position.x,target.x)),
                                           Fixed::raw(satSub32(position.z,target.z))).v);
    if (random(100)==0) {
        const bool subtract=random(2)!=0;
        const int turn=int(random(2048));
        angle=uint16_t(subtract ? angle-turn : angle+turn);
    }
    const double dx=double(position.x)-target.x,dz=double(position.z)-target.z;
    int radius=truncSat32(std::sqrt(dx*dx+dz*dz))>>16;   // past 32768 px a bare cast is undefined
    if (std::abs(radius-distance)>16 || random(100)==0)
        radius=std::abs(distance+int(random(8))-3);
    return {{satAdd32(target.x,retailScaledSine(angle,Fixed::fromInt(radius).v)),target.y,
             satAdd32(target.z,retailScaledCosine(angle,Fixed::fromInt(radius).v))},0x68,angle,0};
}

// 41ef00 stages 4--6: construction hover point around the placed site. Retail
// subtracts the first radius draw and adds the second; keeping the helper shared
// prevents queued and placed flying builders from drifting apart.
template<class Random>
RetailFlightGoal retailConstructionHoverGoal(RetailFlightVector position,
        RetailFlightVector site, int buildDistance, Random random) {
    uint16_t angle=uint16_t(retailDirection(Fixed::raw(satSub32(position.x,site.x)),
                                           Fixed::raw(satSub32(position.z,site.z))).v);
    const int subtractAngle=int(random(0x2492));
    const int addAngle=int(random(0x2492));
    angle=uint16_t(int(angle)-subtractAngle+addAngle);
    const int inward=int(random(8));
    const int outward=int(random(8));
    const int radius=buildDistance-inward+outward;
    const int32_t fixedRadius=Fixed::fromInt(radius).v;
    RetailFlightGoal goal;
    goal.point={satAdd32(site.x,retailScaledSine(angle,fixedRadius)),
                0,
                satAdd32(site.z,retailScaledCosine(angle,fixedRadius))};
    goal.flags=0x60;goal.heading=angle;goal.radius=0;
    return goal;
}

struct RetailLandingState {
    RetailMissionState mission;
    RetailFlightVector anchor;
    uint32_t angle=0,parity=0;
    bool canceled=false;
};

// Ordinary 416cd0 landing path, after boundary recovery/diversion. Host owns
// weapon/script notifications, terrain feasibility and controller installation.
template<class Host>
int retailLanding(RetailLandingState& state,uint32_t events,RetailFlightVector position,
        uint16_t heading,int16_t footX,int16_t footZ,bool hasMover,bool canFly,Host& host) {
    auto& m=state.mission;
    if (state.canceled || (events&0x200)) return 5;
    switch (m.stage) {
    case 0:
        if (!hasMover || !canFly) return 7;
        if (!state.anchor.x && !state.anchor.y && !state.anchor.z) state.anchor=position;
        state.angle=host.random(65536); state.parity=state.angle&1;
        host.initialize(); return 1;
    case 1:
        if (host.velocityPercent()>10) {
            auto point=position;
            point.x=satSub32(point.x,retailScaledSine(heading,32*65536));
            point.z=satSub32(point.z,retailScaledCosine(heading,32*65536));
            host.install(RetailFlightGoal{point}); m.waitMask=0x700;
        }
        return 1;
    case 2:
        if (host.landable(position)) {
            host.touchdown();
            auto point=position; point.y=Fixed::fromInt(std::min(host.groundHeight(position),511)).v;
            host.install(RetailFlightGoal{point,0x28,0,0});
            m.waitMask=0x700; host.deactivate(); return 1;
        } else {
            bool orbit=false;
            const auto point=retailLandingSearch(position,state.anchor,footX,footZ,events,state.angle,orbit,
                [&](int n){return host.random(n);},[&](auto p){return host.landable(p);});
            host.install(RetailFlightGoal{point,uint16_t(orbit ? 0x30 : 0x20),0,int16_t(orbit ? 64 : 0)});
            if (orbit) m.waitMask|=0x700; else m.waitMask=0x700;
            return 2;
        }
    case 3:
        if (events&0x100) { host.finish(); return 5; }
        return 8;
    default: return 7;
    }
}

// 41a47e..41a613: patrol overshoot and acceptance radius. Repeated random
// expressions in the original comparisons draw again; they are not cached.
template<class Random>
RetailFlightGoal retailFlightPatrolGoal(RetailFlightVector position,
        RetailFlightVector waypoint, int32_t nominalSpeed, Random random) {
    const int32_t dx=satSub32(position.x,waypoint.x), dz=satSub32(position.z,waypoint.z);
    const uint16_t heading=uint16_t(retailDirection(Fixed::raw(dx),Fixed::raw(dz)).v);
    const int32_t distance=truncSat32(std::sqrt(double(dx)*dx+double(dz)*dz));
    const int pixels=int16_t(uint32_t(distance)>>16);
    int ahead=(int(random(5))+8)*16, spread=int(random(5))*16;
    if (ahead+spread>pixels*3) ahead=spread=0;
    const uint16_t angle=uint16_t(random(65536));
    RetailFlightGoal goal;
    goal.point={satSub32(satSub32(waypoint.x,retailScaledSine(heading,ahead*65536)),retailScaledSine(angle,spread*65536)),
                waypoint.y,
                satSub32(satSub32(waypoint.z,retailScaledCosine(heading,ahead*65536)),retailScaledCosine(angle,spread*65536))};
    const int base=nominalSpeed>3*65536 ? 15 : 10;
    const int third=pixels/3;
    auto candidate=[&] { return (int(random(10))+base)*16; };
    int radius=third<candidate() ? third : candidate();
    if (radius<80) radius=80;
    else radius=third<candidate() ? third : candidate();
    goal.flags=0x30;
    goal.radius=int16_t(radius);
    return goal;
}

struct RetailFlightNavigation {
    RetailFlightVector destination, velocity;
    uint16_t heading = 0;
    // Ours: the destination is a direction (VTOL_Evade's line, see the top of
    // this file) and is subtracted in wrapped arithmetic, as retail does. It
    // stays with the destination, which the mover flies on to after its
    // controller has gone.
    bool wraps = false;
};

// 524af0: destination motion is measured before the cruise-height override.
// A close controller can supply a heading; otherwise the previous heading is
// retained inside 16 pixels and the destination direction is used farther out.
// `wraps`: this destination is a direction (RetailFlightNavigation::wraps);
// `previousWraps`: the one it replaces was, so the step between them is taken
// in wrapped arithmetic too.
inline RetailFlightNavigation retailFlightNavigation(RetailFlightVector position,
        RetailFlightVector previousDestination, RetailFlightVector destination,
        uint16_t previousHeading, int32_t cruiseHeight, bool cruisingController,
        bool hasHeading, uint16_t controllerHeading, bool wraps = false, bool previousWraps = false) {
    const auto step = [&](int32_t a, int32_t b) {
        return wraps || previousWraps ? retailWrappedSub(a, b) : satSub32(a, b);
    };
    const auto reach = [&](int32_t a, int32_t b) { return wraps ? retailWrappedSub(a, b) : satSub32(a, b); };
    RetailFlightNavigation result{destination,
        {step(destination.x, previousDestination.x),
         step(destination.y, previousDestination.y),
         step(destination.z, previousDestination.z)}, previousHeading, wraps};
    const int32_t dx = reach(position.x, destination.x);
    const int32_t dz = reach(position.z, destination.z);
    const int32_t distance = truncSat32(std::sqrt(double(dx) * dx + double(dz) * dz));
    if (distance > 160 * 65536 || cruisingController) result.destination.y = cruiseHeight;
    if (distance <= 320 * 65536 && hasHeading) result.heading = controllerHeading;
    else if (distance > 16 * 65536)
        result.heading = uint16_t(retailDirection(Fixed::raw(dx), Fixed::raw(dz)).v);
    return result;
}

// 4da803..4dad1a. Inputs are raw 16.16 values after terrain multipliers.
// The navigator supplies a destination and a velocity for that destination.
// Heading, visual banking, collision and position commit belong to the host.
inline RetailFlightVector retailFlightVelocity(RetailFlightVector velocity,
        RetailFlightVector position, RetailFlightVector destination,
        RetailFlightVector destinationVelocity, int32_t previousSpeed,
        int32_t maximumSpeed, int32_t acceleration, int32_t lateralLimit,
        uint16_t heading, bool directControl = false, bool wraps = false) {
    assert(maximumSpeed > 0 && acceleration >= 0 && lateralLimit >= 0);
    constexpr double unit = 1.0 / 65536;
    const int32_t accelerationLimit = std::min(acceleration, maximumSpeed);
    const float limit = float(double(accelerationLimit) * unit);
    const int32_t retention = 65536 - int32_t(double(accelerationLimit) / maximumSpeed * 65536);
    auto scale = [](int32_t value, int32_t factor) {
        return int32_t((int64_t(value) * factor) >> 16);
    };
    velocity = {scale(velocity.x, retention), scale(velocity.y, retention), scale(velocity.z, retention)};
    const float horizontal = float(std::sqrt(double(velocity.x) * velocity.x +
                                             double(velocity.z) * velocity.z) * unit);
    const float lateral = float(double(lateralLimit) * unit);
    if (horizontal > lateral) {
        const int32_t ratio = int32_t(double(lateral) / horizontal * 65536);
        const int32_t excess = int32_t((double(horizontal) - lateral) * 65536);
        velocity.x = scale(velocity.x, ratio) - retailScaledSine(heading, excess);
        velocity.z = scale(velocity.z, ratio) - retailScaledCosine(heading, excess);
    }
    // `wraps`: the destination is a direction (RetailFlightNavigation::wraps).
    // The velocities are no positions: their difference stays retail's.
    const auto reach = [&](int32_t a, int32_t b) { return wraps ? retailWrappedSub(a, b) : satSub32(a, b); };
    const int32_t dx = reach(position.x, destination.x);
    const int32_t dy = reach(position.y, destination.y);
    const int32_t dz = reach(position.z, destination.z);
    const int32_t relativeX = retailWrappedSub(velocity.x, destinationVelocity.x);
    const int32_t relativeZ = retailWrappedSub(velocity.z, destinationVelocity.z);
    if (!directControl) {
        int32_t verticalLimit = std::max(65536, previousSpeed >> 2);
        if (dy > 0 && std::abs(int64_t(dx)) < dy / 4 && std::abs(int64_t(dz)) < dy / 4)
            verticalLimit *= 2;
        velocity.y = int32_t(std::clamp(-int64_t(dy), -int64_t(verticalLimit), int64_t(verticalLimit)));
    }
    const float distance = float(std::sqrt(double(dx) * dx + double(dz) * dz) * unit);
    const float approach = float(-std::sqrt(double(limit) * 2 / std::max(distance, 8.0f)));
    double correctionX = double(dx) * approach * unit - double(relativeX) * unit;
    const double correctionZ = double(dz) * approach * unit - double(relativeZ) * unit;
    float storedZ = float(correctionZ);
    const double length = std::sqrt(correctionX * correctionX + correctionZ * correctionZ);
    if (length > limit) {
        const double ratio = double(limit) / length;
        correctionX *= ratio;
        storedZ = float(double(storedZ) * ratio);
    }
    velocity.x += int32_t(correctionX * 65536);
    velocity.z += int32_t(double(storedZ) * 65536);
    return velocity;
}

} // namespace ta::sim
