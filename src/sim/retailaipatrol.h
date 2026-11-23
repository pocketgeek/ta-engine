#pragma once

#include "retailmotion.h"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>

namespace tak::sim {

// 40d7ad..40db3f, when the threat-target candidate list is empty. The
// placement host runs before separation rejection, preserving query order.
template<class Random,class Placeable>
std::optional<std::array<std::pair<int32_t,int32_t>,2>> retailAiPatrolPoints(
        int32_t centerX,int32_t centerZ,int radius,int width,int height,
        Random random,Placeable placeable) {
    std::array<std::pair<int32_t,int32_t>,2> points{};
    for (unsigned point=0;point<2;++point) {
        bool accepted=false;
        for (unsigned attempt=0;attempt<10;++attempt) {
            const uint32_t inner=random(radius/2);
            const uint32_t distance=(inner+uint32_t(random(radius)))<<16;
            const uint16_t angle=uint16_t(random(65536));
            const int32_t x=std::bit_cast<int32_t>(uint32_t(centerX)-uint32_t(
                retailScaledSine(angle,std::bit_cast<int32_t>(distance))));
            const int32_t z=std::bit_cast<int32_t>(uint32_t(centerZ)-uint32_t(
                retailScaledCosine(angle,std::bit_cast<int32_t>(distance))));
            if (x<0 || z<0 || int64_t(x)>=int64_t(width)*65536 || int64_t(z)>=int64_t(height)*65536)
                continue;
            if (!placeable(x,z)) continue;
            if (point) {
                const int64_t dx=std::bit_cast<int32_t>(uint32_t(points[0].first)-uint32_t(x));
                const int64_t dz=std::bit_cast<int32_t>(uint32_t(points[0].second)-uint32_t(z));
                if (std::bit_cast<int32_t>(uint32_t((dx*dx)>>32)+uint32_t((dz*dz)>>32))<10000)
                    continue;
            }
            points[point]={x,z};accepted=true;break;
        }
        if (!accepted) return {};
    }
    return points;
}

} // namespace tak::sim
