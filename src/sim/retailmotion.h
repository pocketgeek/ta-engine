#pragma once

#include "sim/fixed.h"
#include <algorithm>
#include <array>
#include <bit>

namespace tak::sim {

struct RetailSteeringPoint { Fixed x, z; };

// 0x4d9bc8..0x4d9ce9. The navigator supplies the segment start and end;
// the mover pulls the aim back along that segment when its end is far away.
// Division truncates toward zero, while the final signed product shifts down.
inline RetailSteeringPoint retailSteeringPoint(Fixed x, Fixed z,
        RetailSteeringPoint start, RetailSteeringPoint end, unsigned mode = 0) {
    const Fixed distance = fxLen(end.x - x, end.z - z);
    const Fixed threshold = Fixed::fromInt(mode == 0 ? 80 : 16);
    if (distance <= threshold) return end;
    const Fixed dx = end.x - start.x, dz = end.z - start.z;
    const Fixed length = fxLen(dx, dz);
    if (length < Fixed::fromInt(1)) return end;
    const int32_t back = fxMin(distance - threshold, length).v;
    const int64_t nx = int64_t(dx.v) * 65536 / length.v;
    const int64_t nz = int64_t(dz.v) * 65536 / length.v;
    return {Fixed::raw(end.x.v - int32_t((nx * back) >> 16)),
            Fixed::raw(end.z.v - int32_t((nz * back) >> 16))};
}

// Retail 0x4da53a..0x4da56e: speed is 16.16 pixels/tick, both angles
// are BAM, and rate is BAM/tick. No conversion to radians or seconds.
constexpr Fixed retailTurnTravel(Fixed speed, uint16_t angle, uint16_t rate) {
    return rate ? Fixed::raw(int32_t(int64_t(speed.v) * angle / rate)) : Fixed();
}

// Retail's observed forward displacement is (-sin(h), -cos(h)); our World
// uses (+sin(h), +cos(h)). Convert only at the import/export boundary.
constexpr Bam retailHeadingToPort(uint16_t heading) { return bamWrap(int32_t(heading) + 32768); }
constexpr uint16_t portHeadingToRetail(Bam heading) { return uint16_t(heading.v + 32768); }

// 4d91b0 reads the requested difference as a signed word: an exact half-turn
// is -32768, unlike the general-purpose bamDiff convention of +32768.
constexpr int32_t retailTurnRequest(Bam wanted,Bam heading) {
    return std::bit_cast<int16_t>(uint16_t(wanted.v-heading.v));
}

namespace detail {
// High precision vectoring angles, generated mathematically. Q32 BAM leaves
// room to round once at the end, unlike the old 16-step integer-BAM CORDIC.
constexpr auto makeDirectionAngles() {
    std::array<int64_t, 48> angles{};
    constexpr long double pi = 3.141592653589793238462643383279502884L;
    long double x = 1;
    for (size_t i = 0; i < angles.size(); ++i, x *= 0.5L) {
        long double sum = pi / 4;
        if (i) {
            sum = 0;
            long double term = x;
            for (int n = 0; n < 64; ++n) {
                sum += term / (2 * n + 1);
                term *= -x * x;
            }
        }
        angles[i] = int64_t(sum * (65536.0L / (2 * pi)) * 4294967296.0L + 0.5L);
    }
    return angles;
}
inline constexpr auto directionAngles = makeDirectionAngles();
// Exact bound on all possible remaining integer CORDIC angle updates. Once
// both ends round to the same BAM, later iterations cannot change the result.
inline constexpr auto directionTails = [] {
    std::array<int64_t,49> tails{};
    for (int i=47;i>=0;--i) tails[size_t(i)]=tails[size_t(i+1)]+directionAngles[size_t(i)];
    return tails;
}();
// Generate mathematical Q13 sine samples, not bytes copied from a retail asset.
// Ten Taylor terms over [0, pi/2] are comfortably below half a Q13 unit.
constexpr auto makeQuarterSine() {
    std::array<int16_t, 129> values{};
    constexpr long double pi = 3.141592653589793238462643383279502884L;
    for (int i = 0; i <= 128; ++i) {
        const long double x = i * pi / 256;
        long double term = x, sum = x;
        for (int n = 1; n < 10; ++n) {
            term *= -x * x / ((2 * n) * (2 * n + 1));
            sum += term;
        }
        values[size_t(i)] = int16_t(sum * 8192 + 0.5L);
    }
    return values;
}

inline constexpr auto quarterSine = makeQuarterSine();
constexpr int sineSample(unsigned index) {
    index &= 511;
    const int sign = index >= 256 ? -1 : 1;
    index &= 255;
    return sign * quarterSine[index <= 128 ? index : 256 - index];
}
}

// Retail 0x53612a rounds atan2 to nearest BAM. Keep the simulation independent
// of host libm and its floating-point environment. Arguments follow atan2(y,x).
inline Bam retailDirection(Fixed y, Fixed x) {
    // Axis-aligned vectors have exact BAM results; no vectoring is needed.
    if (!x.v) return Bam(y.v>0 ? 16384 : y.v<0 ? 49152 : 0);
    if (!y.v) return Bam(x.v<0 ? 32768 : 0);
    int64_t vx = int64_t(x.v) * (int64_t(1) << 28);
    int64_t vy = int64_t(y.v) * (int64_t(1) << 28);
    int64_t angle = 0;
    if (vx < 0) {
        angle = (vy >= 0 ? int64_t(32768) : -int64_t(32768)) * (int64_t(1) << 32);
        vx = -vx;
        vy = -vy;
    }
    for (unsigned i = 0; i < detail::directionAngles.size() && vy; ++i) {
        const int64_t dx = vx >> i, dy = vy >> i;
        if (vy > 0) {
            vx += dy; vy -= dx; angle += detail::directionAngles[i];
        } else {
            vx -= dy; vy += dx; angle -= detail::directionAngles[i];
        }
        if (i>=14) {
            const int64_t tail=detail::directionTails[i+1];
            const int64_t low=(angle-tail+(int64_t(1)<<31))>>32;
            const int64_t high=(angle+tail+(int64_t(1)<<31))>>32;
            if (low==high) return bamWrap(int32_t(low));
        }
    }
    return bamWrap(int32_t((angle + (int64_t(1) << 31)) >> 32));
}

// 0x4dbf90: squared distance to the infinite segment line, in whole pixels.
// Axis-aligned and degenerate segments use separate high-word products.
inline int32_t retailLineDistance(RetailSteeringPoint p, RetailSteeringPoint a,
                                  RetailSteeringPoint b) {
    const int64_t x = int64_t(p.x.v) - a.x.v, z = int64_t(p.z.v) - a.z.v;
    if (a.x == b.x) {
        const auto dx = int32_t((x * x) >> 32);
        return a.z == b.z ? dx + int32_t((z * z) >> 32) : dx;
    }
    if (a.z == b.z) return int32_t((z * z) >> 32);
    const double sx = (double(a.x.v) / 65536.0), sz = (double(a.z.v) / 65536.0);
    const double slope = ((double(b.z.v) / 65536.0) - sz) / ((double(b.x.v) / 65536.0) - sx);
    const double inverse = 1.0 / slope;
    const double projected = (inverse * (double(p.x.v) / 65536.0) + slope * sx + (double(p.z.v) / 65536.0) - sz)
                           / (inverse + slope);
    const double dx = (double(p.x.v) / 65536.0) - projected;
    const double dz = dx * inverse;
    return int32_t(dx * dx + dz * dz);
}

// 0x4dbd19..0x4dbf7e, after the eight neighboring footprints
// have grade >= 4. The caller repeats the scan after removing a corner.
inline bool retailPruneGroundCorner(Fixed x, Fixed z, RetailSteeringPoint p0,
        RetailSteeringPoint p1, RetailSteeringPoint p2, Bam heading, uint16_t rate,
        bool boat=false, uint8_t speedMode=0) {
    if (p1.x == p2.x && p1.z == p2.z) return false;
    const int distance = fxLen(p1.x - x, p1.z - z).floorInt();
    if (distance >= 144) return false;
    if (distance < 32) return true;
    const int d0 = retailLineDistance({x,z}, p0, p1);
    const int d1 = retailLineDistance({x,z}, p1, p2);
    const int a0 = std::abs(bamDiff(retailDirection(p1.x-x,p1.z-z),heading))*(boat?5:1);
    int a1 = std::abs(bamDiff(retailDirection(p2.x-x,p2.z-z),heading));
    if (rate >= 1000) a1 = 0;
    else if (rate >= 500) a1 /= 2;
    return (d0 >= d1 && a0 >= a1) || (int64_t(d0)*2 >= d1 && a0 >= a1*2) ||
           (boat && speedMode==0 && distance<160);
}

// 0x4d9d15..0x4da607: braking looks at three navigator points, separately
// from the steering aim. Near point 1 both acceleration and braking are
// capped at one third of the terrain-adjusted individual maximum speed.
inline Fixed retailGroundAcceleration(Fixed x, Fixed z,
        RetailSteeringPoint p0, RetailSteeringPoint p1, RetailSteeringPoint p2,
        Bam heading, Fixed speed, Fixed maximum, Fixed accel, Fixed brake,
        uint16_t turnRate, const RetailSteeringPoint* knownAim=nullptr,
        int knownDirection=-1) {
    const bool nearPoint = fxLen(p1.x - x, p1.z - z) <= Fixed::fromInt(80);
    const Fixed cap = nearPoint ? Fixed::raw(maximum.v / 3) : maximum;
    brake = fxMin(brake, cap);
    accel = fxMin(accel, cap);
    const auto aim = knownAim ? *knownAim : retailSteeringPoint(x, z, p0, p1);
    // The executable takes the absolute difference of unsigned retail angles,
    // not the shortest wrapped difference used by the actual turn operation.
    const int direction = knownDirection>=0 ? knownDirection : retailDirection(x - aim.x, z - aim.z).v;
    const int angle = std::abs(direction - int(portHeadingToRetail(heading)));
    const int64_t arc = retailTurnTravel(speed, uint16_t(angle), turnRate).v;
    // (An unsigned product: a navigator point 46,341 px or more away on one axis squares past int64.)
    auto squareHigh = [](int64_t n) { return uint32_t((uint64_t(n) * uint64_t(n)) >> 32); };
    auto distanceHigh = [&](RetailSteeringPoint p) {
        return int32_t(squareHigh(int64_t(p.x.v) - x.v) + squareHigh(int64_t(p.z.v) - z.v));
    };
    const int32_t arcSquared = int32_t(squareHigh(arc) * 4);
    const int32_t stop = brake.v > 0
        ? int32_t(double((int64_t(speed.v) * speed.v) >> 16) / (2.0 * brake.v) * 65536.0) : 0;
    return distanceHigh(aim) <= arcSquared || distanceHigh(p2) <= int32_t(squareHigh(stop))
        ? -brake : accel;
}

// Retail 0x5360bf / 0x5360f3: a 512-sample Q13 table, a +32 BAM bias
// before binning, then signed rounding by adding 4096 before shifting.
constexpr int32_t retailScaledSine(uint16_t heading, int32_t magnitude) {
    const unsigned index = (unsigned(heading) + 32) >> 7;
    return int32_t((int64_t(detail::sineSample(index)) * magnitude + 4096) >> 13);
}
constexpr int32_t retailScaledCosine(uint16_t heading, int32_t magnitude) {
    return retailScaledSine(uint16_t(heading + 16384), magnitude);
}

// 4d95f0: movement speed mode scales the maximum, then signed pitch selects
// a quantized ramp limit. The percentage is converted to Q16 before multiplying.
inline Fixed retailGroundPitchCap(Fixed maximum,uint16_t pitch) {
    const int band=std::clamp(int(std::bit_cast<int16_t>(pitch))>>11,-5,5);
    const int percent=band== -5?25:band< -1?115+15*band:
                      band<=0?100:band<=3?100-25*band:40-5*band;
    return maximum*Fixed::raw(percent*65536/100);
}
inline Fixed retailGroundSpeedCap(Fixed maximum,uint16_t pitch,uint8_t mode=0) {
    if (mode==1) maximum=maximum*Fixed::raw(65536*667/1000);
    else if (mode==2) maximum=maximum*Fixed::raw(65536*333/1000);
    return retailGroundPitchCap(maximum,pitch);
}
constexpr SinCos retailGroundStep(Bam portHeading, Fixed speed) {
    const uint16_t heading = portHeadingToRetail(portHeading);
    // 0x4dab0d / 0x4dab2b negate AFTER rounding the product.
    return {Fixed::raw(-retailScaledSine(heading, speed.v)),
            Fixed::raw(-retailScaledCosine(heading, speed.v))};
}

struct RetailGroundScan {
    uint32_t nextTick = 0;
    uint8_t movementMode = 0, speedMode = 0;
};

// 4dba80..4dbd19: placement and visibility are supplied by the world. The
// returned modes replace the previous modes, even when the scan is disabled.
template<class Grade, class Visible>
RetailGroundScan retailGroundScan(Fixed x, Fixed y, Fixed z, Bam heading,
        uint32_t tick, uint8_t halfCellTicks, bool boat, int16_t footprintX,
        int16_t sight, bool disabled, Grade grade, Visible visible) {
    RetailGroundScan result{tick+halfCellTicks};
    if (disabled) return result;
    if (boat) {
        const auto step=retailGroundStep(heading,Fixed::fromInt(16));
        Fixed px=x,pz=z;
        const int limit=(int(footprintX)+1)*16+int(sight);
        for (int distance=0;distance<limit;distance+=16) {
            px+=step.s;pz+=step.c;
            const int value=grade(px,y,pz);
            if (value<=4) {
                result.speedMode=distance<=160 && value<4 ? 2 : 1;
            } else if (!visible(px,y,pz)) result.speedMode=1;
            if (result.speedMode) {
                result.nextTick+=uint32_t(halfCellTicks)*result.speedMode;
                break;
            }
        }
    }
    for (int ox=-1;ox<=1;++ox)
        for (int oz=-1;oz<=1;++oz)
            if ((ox || oz) && grade(x+Fixed::fromInt(ox*16),y,z+Fixed::fromInt(oz*16))<4) {
                result.movementMode=1;
                return result;
            }
    return result;
}

// Boat visibility uses projected 32px cells, including the stored height.
template<class WordAt>
bool retailBoatScanVisible(Fixed x, Fixed y, Fixed z, int width, int height,
                          uint8_t player, WordAt wordAt) {
    const int cx=x.floorInt()>>5;
    const int cz=(z.floorInt()-(y.floorInt()>>1))>>5;
    if (uint32_t(cx)>=uint32_t(width) || uint32_t(cz)>=uint32_t(height)) return false;
    return (wordAt(cx,cz)&(uint32_t(1)<<(player&31)))!=0;
}

} // namespace tak::sim
