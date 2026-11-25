#pragma once
#include "sim/retailpiecepose.h"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>

namespace ta::sim {
// 52a6f3: environmental part of the projectile cell test. Feature height
// is relative to the cell minimum; callers resolve multi-cell feature anchors.
inline bool retailProjectileHitsEnvironment(int32_t y,int32_t& verticalSpeed,
        uint32_t flags,uint8_t minimum,uint8_t sea,std::optional<uint8_t> featureHeight,
        bool waterPass) {
    if(flags&0x800)return false;
    const int height=std::bit_cast<int16_t>(uint16_t(uint32_t(y)>>16));
    if(featureHeight && height<=int(minimum)+*featureHeight)return true;
    if(height<=minimum) {
        if(!(flags&0x1000))return true;
        verticalSpeed=-(verticalSpeed/4);
        return false;
    }
    return !(flags&0x2000) && height<sea && !waterPass;
}

// 52a519: optional interception proximity to another projectile. Each squared
// fixed-point delta is truncated separately before the signed, wrapping sum.
inline bool retailProjectileProximity(const std::array<int32_t,3>& point,
        const std::array<int32_t,3>& target,uint16_t radius) {
    uint32_t distance=0;
    for(unsigned axis=0;axis<3;++axis) {
        const int32_t delta=std::bit_cast<int32_t>(uint32_t(point[axis])-uint32_t(target[axis]));
        distance+=uint32_t(uint64_t(int64_t(delta)*delta)>>32);
    }
    return std::bit_cast<int32_t>(distance)<std::bit_cast<int32_t>(uint32_t(radius)*radius);
}

} // namespace ta::sim
