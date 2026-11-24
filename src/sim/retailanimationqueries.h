#pragma once
#include <algorithm>
#include <cstdint>
#include "sim/fixed.h"
#include "sim/retailmotion.h"
#include "sim/retailpiecepose.h"

namespace ta::sim {
// 52c4e0: direct/guided weapon heading and pitch from a 16.16 aim vector.
// Pitch uses signed whole-word height and horizontal length, unlike heading.
// (The length is kept whole here: retail's signed word turns negative at
// 32768 px, farther than any retail map is wide. Identical below that.)
inline std::array<uint16_t,2> retailDirectAim(int32_t dx,int32_t dy,int32_t dz,
                                            uint16_t nativeHeading) {
    const uint64_t horizontal=isqrt64(uint64_t(int64_t(dx)*dx)+uint64_t(int64_t(dz)*dz));
    const int32_t height=std::bit_cast<int16_t>(uint16_t(uint32_t(dy)>>16));
    const int32_t distance=int32_t(horizontal>>16);
    return {uint16_t(retailDirection(Fixed::raw(dx),Fixed::raw(dz)).v-nativeHeading),
            uint16_t(retailDirection(Fixed::raw(-height),Fixed::raw(distance)).v)};
}

// 4da620: accumulated acceleration drives aircraft attitude, including the
// native quirk that both scale terms use the rotated lateral component.
inline std::array<uint16_t,2> retailFlightAttitude(std::array<int32_t,3>& retained,
        const std::array<int32_t,3>& acceleration,uint16_t heading,
        int32_t bankScale,int32_t pitchScale,int32_t gravity) {
    for(size_t axis=0;axis<3;++axis)
        retained[axis]=std::bit_cast<int32_t>(uint32_t((int64_t(retained[axis])*0xf333)>>16)+
                                            uint32_t(acceleration[axis]));
    int32_t lateral=retained[0],forward=retained[2];
    retailRotatePair(lateral,forward,heading);
    const int32_t denominator=int32_t(double(gravity)*19.99877937137626);
    const int32_t negative=std::bit_cast<int32_t>(0u-uint32_t(lateral));
    auto angle=[&](int32_t scale) {
        const int32_t value=std::bit_cast<int32_t>(uint32_t((int64_t(negative)*scale)>>16));
        return uint16_t(retailDirection(Fixed::raw(value),Fixed::raw(denominator)).v);
    };
    return {angle(bankScale),angle(pitchScale)};
}

// 4db350: MoveRate is a tier (0..3), including slow motion when only
// turning. Refused and attached movers report stopped, even with retained speed.
inline uint32_t retailAnimationMoveRate(int32_t speed,int16_t turn,int32_t horizontal,
        int32_t slow,int32_t fast,bool blocked,bool attached) {
    if (blocked || attached || (!speed && !turn)) return 0;
    return horizontal<=slow ? 1u : 2u+uint32_t(horizontal>fast);
}

// TotalA.exe 0x43da70: TA reports a movement-tier change (the same 0..3 tier,
// kept in bits 2-3 of unit+0x110) to the unit script by name, with no
// arguments: tier 0 -> StopMoving; a tier from 0 -> StartMoving; then, for a
// nonzero tier, MoveRate1/MoveRate2/MoveRate3. (Kingdoms' MoveRate(tier) and
// TurnDirection do not exist in TotalA.exe.) `call(name)` runs one call-in.
template<class Call>
inline void retailMovementCallins(uint32_t previous,uint32_t rate,Call&& call) {
    if (rate==previous) return;
    if (!rate) call("StopMoving");
    else if (!previous) call("StartMoving");
    static constexpr const char* kRate[3]={"MoveRate1","MoveRate2","MoveRate3"};
    if (rate>=1 && rate<=3) call(kRate[rate-1]);
}

// 4dc600: setSFXoccupy keeps the previous value in the deep, partially
// submerged band. The tests are ordered: fully submerged overrides floating.
inline uint32_t retailAnimationOccupancy(uint32_t previous,uint8_t mode,
        int16_t height,uint8_t sea,uint8_t waterline,int16_t modelTop) {
    if ((mode&3)!=1) return (mode&3)==2 ? 5u : 0u;
    if (height>sea) return 4;
    auto result=previous;
    if (int(height)-sea>-5) result=1;
    if (int(height)+waterline==sea) result=2;
    if (int(height)+modelTop<sea) result=3;
    return result;
}

// 4dc100: normalize the mover's horizontal vector by its individual,
// terrain-adjusted speed, round to nearest, then clamp to 0..100.
inline int32_t retailHorizontalAnimationPercent(int32_t dx,int32_t dz,int32_t maximum,
                                                bool blocked,bool attached) {
    if(blocked || attached || maximum<=0) return 0;
    const uint64_t length=isqrt64(uint64_t(int64_t(dx)*dx)+uint64_t(int64_t(dz)*dz));
    if(length>0x7fffffffu) return 0;
    return int32_t(std::min(100.0,double(length)*100.0/double(maximum)+0.5));
}

// 4dc3f0: GET 33 is signed percentage, not BAM units. Terrain scaling is
// truncated to a word independently for both turn rates before taking the max.
inline int32_t retailTurnAnimationPercent(int16_t turn,uint16_t movingRate,
        uint16_t pivotRate,int32_t multiplier,bool attached) {
    if(attached) return 0;
    const uint16_t moving=uint16_t(int64_t(movingRate)*multiplier/65536);
    const uint16_t pivot=uint16_t(int64_t(pivotRate)*multiplier/65536);
    const uint16_t maximum=std::max(moving,pivot);
    // 5d3d54 converts to a 64-bit integer and returns its low word.
    // A zero denominator produces the integer-indefinite value whose low
    // word is zero, including the stationary 0/0 case.
    if(!maximum) return 0;
    const double percent=double(turn)*100.0/maximum;
    return int32_t(std::clamp(percent+(turn<0 ? -0.5 : 0.5),-100.0,100.0));
}

// Native GET 30, 4dc1f0..4dc3e9, airborne branch. Inputs are 16.16
// vertical velocity and effective base speed; the denominator is twice
// max(1, baseSpeed/4), with native integer truncation before division.
inline int32_t retailFlightVerticalPercent(int32_t velocity,int32_t baseSpeed,
                                          bool blocked,bool attached) {
    if(blocked || attached) return 0;
    const int32_t quarter=(baseSpeed&~3)<0x40000 ? 0x10000 : baseSpeed>>2;
    const double percent=double(velocity)*100.0/double(quarter*2);
    return int32_t(std::clamp(percent+(velocity<0 ? -0.5 : 0.5),-100.0,100.0));
}
}
