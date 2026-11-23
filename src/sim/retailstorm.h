#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace tak::sim {
struct RetailStormLaunch {
    std::array<int32_t,3> position{}, baseVelocity{};
    std::array<float,2> variation{};
};

// The native x87 integer helper returns int64's low word, including zero for
// invalid/out-of-range conversion. All finite launch values fit int64 here.
inline int32_t retailStormInteger(double value) {
    if(!(value>=-0x1p63 && value<0x1p63))return 0;
    return std::bit_cast<int32_t>(uint32_t(int64_t(value)));
}

// 52f970: source and stored aim point are signed whole-word coordinates for
// normalization. X direction crosses a float boundary; Z remains double.
// The caller supplies the terrain height at the returned launch X/Z.
inline RetailStormLaunch retailStormLaunch(std::array<int32_t,3> source,
        std::array<int32_t,3> aim,uint32_t speed,uint32_t substeps,int32_t maxVariation) {
    const auto whole=[](int32_t value) { return int32_t(std::bit_cast<int16_t>(uint16_t(uint32_t(value)>>16))); };
    const int32_t dx=whole(aim[0])-whole(source[0]),dz=whole(aim[2])-whole(source[2]);
    const int32_t square=std::bit_cast<int32_t>(uint32_t(dx)*uint32_t(dx)+uint32_t(dz)*uint32_t(dz));
    const double invalid=std::bit_cast<double>(uint64_t(0xfff8000000000000));
    const double length=square>0 ? std::sqrt(double(square)) : invalid;
    const float x=float(double(dx)/length);
    const double z=double(dz)/length;
    const uint32_t perTick=speed*substeps;
    RetailStormLaunch result;
    result.position={std::bit_cast<int32_t>(uint32_t(source[0])+uint32_t(retailStormInteger(double(x)*32*65536))),
        0,std::bit_cast<int32_t>(uint32_t(source[2])+uint32_t(retailStormInteger(z*32*65536)))};
    result.baseVelocity={retailStormInteger(double(perTick)*double(x)),0,retailStormInteger(double(perTick)*z)};
    result.variation={float(double(maxVariation)*z),float(double(maxVariation)*double(x))};
    return result;
}
} // namespace tak::sim
