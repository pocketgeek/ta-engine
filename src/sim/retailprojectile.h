#pragma once
#include "sim/retailpiecepose.h"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>

namespace ta::sim {
// TotalA.exe 0x49b28a..0x49b3cd: environmental part of the projectile cell
// test, on the whole-px height (the y word +0xa). unitsonly skips it
// (0x49b294). A feature strikes when its height over the cell minimum is
// above the shot (0x49b32a..0x49b33f, strict); the ground when the cell
// minimum is (0x49b36d..0x49b37d, strict: a shot level with it flies on), a
// groundbounce shot bouncing there with its vertical speed sar 2, negated
// (0x49b38c..0x49b392); then water below the sea, unless waterweapon or the
// map's no-sea-level-trigger (0x49b3a1..0x49b3c8). Feature height is
// relative to the cell minimum; callers resolve multi-cell feature anchors.
inline bool retailProjectileHitsEnvironment(int32_t y,int32_t& verticalSpeed,
        uint32_t flags,uint8_t minimum,uint8_t sea,std::optional<uint8_t> featureHeight,
        bool waterPass) {
    if(flags&0x800)return false;
    const int height=std::bit_cast<int16_t>(uint16_t(uint32_t(y)>>16));
    if(featureHeight && int(minimum)+*featureHeight>height)return true;
    if(int(minimum)>height) {
        if(!(flags&0x1000))return true;
        verticalSpeed=std::bit_cast<int32_t>(0u-uint32_t(verticalSpeed>>2));
        return false;
    }
    return !(flags&0x2000) && height<sea && !waterPass;
}

// TotalA.exe 0x49b106..0x49b1a4: an interceptor shot's proximity to the shot
// it chases (+0x56) within the weapon's word +0xd6. Each squared fixed-point
// delta keeps its high dword (0x4e4400 / 0x4e43d0 by 32) before the wrapping
// sum, compared signed (0x49b1a4 jge).
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
