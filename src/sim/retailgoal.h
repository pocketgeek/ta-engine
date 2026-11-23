#pragma once
#include "fixed.h"
#include "retailcost.h"
#include "retailmotion.h"
#include <algorithm>
#include <cstdint>
#include <bit>
#include <utility>

namespace tak::sim {

// 4e2500: constructor stores world tolerance and a separately rounded cell
// radius. Preserve 32-bit multiply/add and signed truncating division.
constexpr int32_t retailCircleRadiusSquared(int32_t radius) {
    if (radius <= 30000)
        return std::bit_cast<int32_t>(uint32_t(radius) * uint32_t(radius) + 128u) / 256;
    const uint32_t cells = uint32_t(radius / 16);
    return std::bit_cast<int32_t>(cells * cells);
}

// Controller query fields, in retail cell coordinates (footprint origins).
// Radius/tolerance fields are stored independently by the controller.
struct RetailCircleGoal {
    int x=0, z=0, tolerance=0, radiusSquared=0;
    bool accepts(int cx, int cz) const {
        const int64_t dx=int64_t(cx)-x, dz=int64_t(cz)-z;
        return dx*dx+dz*dz <= radiusSquared;
    }
    int distance(int cx, int cz) const {
        return retailGoalDistance(cx-x,cz-z,tolerance);
    }
    template<class Emit> void enumerate(Emit emit) const { emit(x,z); }
};

struct RetailRectGoal {
    int minX=0, maxX=0, minZ=0, maxZ=0;
    bool operator==(const RetailRectGoal&) const = default;
    // 4e38e0: clamp to the perimeter. Interior ties prefer left, right,
    // bottom, then top; this ordering affects the initial steering segment.
    std::pair<int,int> navigationCell(int x, int z) const {
        const int cx=std::clamp(x,minX,maxX), cz=std::clamp(z,minZ,maxZ);
        if (x<=minX || x>=maxX || z<=minZ || z>=maxZ) return {cx,cz};
        const int left=x-minX, right=maxX-x, top=z-minZ, bottom=maxZ-z;
        if (left<=right && left<=top && left<=bottom) return {minX,z};
        if (right<=top && right<=bottom) return {maxX,z};
        return {x,bottom<=top ? maxZ : minZ};
    }
    bool accepts(int x, int z) const {
        return ((x==minX || x==maxX) && z>=minZ && z<=maxZ)
            || ((z==minZ || z==maxZ) && x>=minX && x<=maxX);
    }
    int distance(int x, int z) const {
        const int dx=std::max({minX-x,0,x-maxX});
        const int dz=std::max({minZ-z,0,z-maxZ});
        if (dx || dz) return 16*std::max(dx,dz)+6*std::min(dx,dz);
        return 16*std::min({x-minX,maxX-x,z-minZ,maxZ-z});
    }
    template<class Emit> void enumerate(Emit emit) const {
        for (int x=minX; x<=maxX; ++x) {
            emit(int16_t(x),int16_t(minZ)); emit(int16_t(x),int16_t(maxZ));
        }
        for (int z=minZ+1; z<maxZ; ++z) {
            emit(int16_t(minX),int16_t(z)); emit(int16_t(maxX),int16_t(z));
        }
    }
};

struct RetailRingGoal {
    int x=0, z=0, innerRadius=0, outerTolerance=0, outerSquared=0;
    bool operator==(const RetailRingGoal&) const = default;
    // 4e33b0: steer outward from the snapped ring center along the unit's
    // current bearing, to the midpoint between the two radii.
    RetailSteeringPoint navigationPoint(Fixed unitX,Fixed unitZ,int footX,int footZ) const {
        const Fixed cx=Fixed::fromInt(x*16+footX*8),cz=Fixed::fromInt(z*16+footZ*8);
        const auto heading=uint16_t(retailDirection(unitX-cx,unitZ-cz).v);
        const int32_t radius=int32_t(uint32_t((innerRadius+outerTolerance)/2)*65536u);
        return {cx+Fixed::raw(retailScaledSine(heading,radius)),
                cz+Fixed::raw(retailScaledCosine(heading,radius))};
    }
    uint64_t squared(int cx, int cz) const {
        const int64_t dx=int64_t(cx)-x, dz=int64_t(cz)-z;
        return uint64_t(dx*dx+dz*dz);
    }
    bool accepts(int cx, int cz) const {
        const auto s=squared(cx,cz);
        return int64_t(s)<=outerSquared &&
            (innerRadius<=0 || s*256>=uint64_t(innerRadius)*uint64_t(innerRadius));
    }
    int distance(int cx, int cz) const {
        const int outside=retailGoalDistance(cx-x,cz-z,outerTolerance);
        if (outside) return outside;
        return std::max(0,innerRadius-int(isqrt64(squared(cx,cz)*256)));
    }
    template<class Emit> void enumerate(Emit emit) const {
        const int32_t radius=((innerRadius+outerTolerance)/2)*65536;
        // The world conversion and footprint offset cancel; preserve its
        // half-cell rounding, ordered samples and repeated cells.
        const auto sample=[&](int32_t dx,int32_t dz) {
            emit(int16_t(x+((int64_t(dx)+0x80000)>>20)),
                 int16_t(z+((int64_t(dz)+0x80000)>>20)));
        };
        sample(0,radius); sample(0,-radius); sample(radius,0); sample(-radius,0);
        for (uint16_t heading : {0x1000,0x2000,0x3000,0x5000,0x6000,0x7000,
                                 0x9000,0xa000,0xb000,0xd000,0xe000,0xf000})
            sample(-retailScaledSine(heading,radius),-retailScaledCosine(heading,radius));
    }
};

} // namespace tak::sim
