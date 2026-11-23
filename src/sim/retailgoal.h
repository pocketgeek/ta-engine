#pragma once
#include "fixed.h"
#include "retailcost.h"
#include "retailmotion.h"
#include <algorithm>
#include <cstdint>
#include <bit>
#include <utility>

namespace ta::sim {

// TotalA.exe circle goal 0x44cf60: accept dx^2+dz^2 <= (r/16)^2, r in px with
// signed truncating division (docs/retail-pathfinding.md section 2.4).
constexpr int32_t retailCircleRadiusSquared(int32_t radius) {
    const uint32_t cells = std::bit_cast<uint32_t>(radius / 16);
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
    // TotalA.exe 0x44dc60: the rect goal's target is the middle of its far
    // (max z) side, wherever the unit stands.
    std::pair<int,int> navigationCell(int, int) const { return {(minX+maxX)/2, maxZ}; }
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
    // TotalA.exe ring goal 0x44d3b0: (in/16)^2 <= d^2 <= (out/16)^2, truncating.
    bool accepts(int cx, int cz) const {
        const auto s=squared(cx,cz);
        const int64_t inner=innerRadius/16;
        return int64_t(s)<=outerSquared && int64_t(s)>=inner*inner;
    }
    // 0x44d840: outside, the octile distance past the outer radius; inside,
    // inner - (18*max + 7*min), floored at 0.
    int distance(int cx, int cz) const {
        const int outside=retailGoalDistance(cx-x,cz-z,outerTolerance);
        if (outside) return outside;
        const int ax=std::abs(cx-x), az=std::abs(cz-z);
        return std::max(0,innerRadius-(18*std::max(ax,az)+7*std::min(ax,az)));
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

} // namespace ta::sim
