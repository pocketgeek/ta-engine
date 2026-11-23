#pragma once
#include "sim/retailflame.h"

namespace tak {
// 48c870 subtracts half raw Y before extracting the signed whole Z.
inline std::array<int,2> retailFlameEndpointPoint(const std::array<int32_t,3>& point) {
    const int32_t projected=std::bit_cast<int32_t>(uint32_t(point[2])-uint32_t(point[1]>>1));
    return {point[0]>>16,int(std::bit_cast<int16_t>(uint16_t(uint32_t(projected)>>16)))};
}
// 4f16e0 first extracts whole Y/Z, then halves whole Y for particle drawing.
inline std::array<int,2> retailFlameParticlePoint(const std::array<int32_t,3>& point) {
    return {point[0]>>16,(point[2]>>16)-((point[1]>>16)>>1)};
}
template<class Viewport,class Visible>
bool retailFlameStreamVisible(const std::array<int32_t,3>& muzzle,
        const std::array<int32_t,3>& aim,Viewport viewport,Visible visible) {
    return (viewport(muzzle) || viewport(aim)) && (visible(muzzle) || visible(aim));
}
} // namespace tak
