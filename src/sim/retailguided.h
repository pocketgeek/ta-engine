#pragma once

#include "sim/detmath.h"
#include "sim/retailmotion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace tak::sim {

// GuidedWeapon (KINGDOMS.icd 0x52c540 / 0x52e030 / 0x52c6d0) shares the
// straight-shot 16.16 XYZ record. Unlike BallisticWeapon it has no gravity:
// update steers the current 3D velocity, then the base mover applies that
// velocity once per weapon substep and checks collision after each move.
struct RetailGuidedShot {
    std::array<int32_t,3> position{};
    std::array<int32_t,3> velocity{};
    std::array<uint16_t,3> angles{}; // roll, yaw, pitch; BAM
};

inline RetailGuidedShot retailGuidedLaunch(const std::array<int32_t,3>& muzzle,
        uint16_t bodyHeading,uint16_t aimHeading,uint16_t pitch,int32_t speedPerSubstep) {
    RetailGuidedShot shot;
    shot.position=muzzle;
    const uint16_t yaw=uint16_t(bodyHeading+aimHeading);
    const int32_t horizontal=retailScaledCosine(pitch,speedPerSubstep);
    shot.velocity={retailScaledSine(yaw,horizontal),
        std::bit_cast<int32_t>(0u-uint32_t(retailScaledSine(pitch,speedPerSubstep))),
        retailScaledCosine(yaw,horizontal)};
    shot.angles={0,aimHeading,uint16_t(0u-pitch)};
    return shot;
}

namespace detail {
inline int32_t retailGuidedTruncate(double raw) {
    if(!std::isfinite(raw) || raw<double(INT32_MIN) || raw>double(INT32_MAX))return INT32_MIN;
    return static_cast<int32_t>(raw); // native 0x5d3d54 truncates toward zero
}
inline int32_t retailGuidedFixed(float value) {
    return retailGuidedTruncate(double(value)*65536.0);
}
inline float retailGuidedFloat(int32_t raw) {
    return float(raw)* (1.0f/65536.0f); // native stores this conversion as float
}
inline float retailGuidedLength(const std::array<float,3>& v) {
    return std::sqrt((v[0]*v[0]+v[1]*v[1])+v[2]*v[2]);
}
// 0x5ad6f0 on the x87 at the game's 53-bit precision (control word 0x027f):
// the float parts square and sum exactly as doubles do.
inline double retailGuidedLength53(const std::array<float,3>& v) {
    const double x=v[0],y=v[1],z=v[2];
    return std::sqrt((x*x+y*y)+z*z);
}
// 0x5ad930, the angle between the current and desired vectors: 0 when either
// has no length, otherwise acos of dot/(|a||b|) with that cosine STORED AS A
// FLOAT (0x5ad99c), 0 above 1 and pi below -1. A target dead ahead therefore
// gets no turn only when the float cosine is exactly 1; one float step under it
// is an angle of 3.45e-4 rad, which every shipped turn rate covers (snap). The
// true angle (atan2 of the cross and dot) is not what native compares.
inline double retailGuidedAngle(const std::array<float,3>& a,const std::array<float,3>& b) {
    const double lengthA=retailGuidedLength53(a);
    if(lengthA==0.0)return 0.0;
    const double lengthB=retailGuidedLength53(b);
    if(lengthB==0.0)return 0.0;
    const float dot=float((double(a[2])*b[2]+double(a[1])*b[1])+double(a[0])*b[0]); // 0x5ad900
    const float cosine=float(double(dot)/(lengthB*double(float(lengthA))));
    if(cosine>1.0f)return 0.0;
    if(cosine<-1.0f)return double(3.14159274f); // 0x5f6460
    // 0x5d8760: atan2(sqrt((1+x)(1-x)), x), here through detmath's double atan.
    const double c=cosine;
    const double s=std::sqrt((1.0+c)*(1.0-c));
    if(c==0.0)return 1.5707963267948966;
    const double angle=detmath::atan(s/c);
    return c>0.0 ? angle : angle+3.141592653589793;
}
inline uint16_t retailGuidedYaw(const std::array<float,3>& v) {
    return uint16_t(static_cast<int32_t>(double(detmath::atan2(v[0],v[2]))*
        10430.378350470464));
}
inline uint16_t retailGuidedPitch(const std::array<float,3>& v,float length) {
    if(length==0.0f)return 0;
    const float y=std::clamp(v[1]/length,-1.0f,1.0f);
    const double yd=double(y);
    const double rem=std::max(0.0,1.0-yd*yd);
    const double angle=detmath::atan(yd==1.0 ? INFINITY : yd==-1.0 ? -INFINITY : yd/std::sqrt(rem));
    return uint16_t(static_cast<int32_t>(angle*10430.378350470464));
}
} // namespace detail

// 0x52e030 steering, followed by 0x52c6d0's substep mover. `turnRadians`
// is the constructor's TurnRate in radians per tick; `speedPerSubstep` is the
// weapon object's +CC fixed speed. The collision callback observes the point
// AFTER each integration step; native stops and impacts only on code 2.
template<class Collision>
inline bool retailGuidedTick(RetailGuidedShot& shot,const std::array<int32_t,3>& target,
        float turnRadians,int32_t speedPerSubstep,uint32_t substeps,
        const std::array<uint16_t,3>& spin,bool updateAngles,Collision collision) {
    std::array<float,3> current{},desired{};
    for(unsigned axis=0;axis<3;++axis) {
        current[axis]=detail::retailGuidedFloat(shot.velocity[axis]);
        const int32_t delta=std::bit_cast<int32_t>(uint32_t(target[axis])-uint32_t(shot.position[axis]));
        desired[axis]=detail::retailGuidedFloat(delta);
    }
    {
        const double angle=detail::retailGuidedAngle(current,desired);
        if(angle!=0.0) {
            if(angle<=double(turnRadians)) {
                // 0x52e108: once within the cap, snap to the target vector and
                // rebuild velocity from subSteps*weaponSpeed. The base mover
                // then applies that larger vector once per substep. 0x5ad720
                // divides each part by the 53-bit length into a float; the
                // product with the speed is truncated without a float store.
                const double length=detail::retailGuidedLength53(desired);
                const uint32_t raw=uint32_t(speedPerSubstep)*substeps;
                const double scale=double(std::bit_cast<int32_t>(raw))*(1.0/65536.0);
                for(unsigned a=0;a<3;++a) {
                    const float unit=float(double(desired[a])/length);
                    shot.velocity[a]=detail::retailGuidedTruncate(double(unit)*scale*65536.0);
                }
            } else {
                // 0x52e16b: quaternion(axis=current x desired, angle=turnRate)
                // rotated the existing velocity; this path preserves its speed.
                // It runs for a zero turn rate too (a Lightning Ball has none):
                // a rotation by 0. 0x5ad8a0 forms each axis part from the exact
                // float products in double and stores it as a float; 0x5ad720
                // divides by its 53-bit length.
                std::array<float,3> axis={
                    float(double(current[1])*desired[2]-double(current[2])*desired[1]),
                    float(double(current[2])*desired[0]-double(current[0])*desired[2]),
                    float(double(current[0])*desired[1]-double(current[1])*desired[0])};
                const double crossLength=detail::retailGuidedLength53(axis);
                if(crossLength==0.0) {
                    // A target exactly in line (antiparallel, or ahead with a
                    // float cosine under 1 and a turn rate under that angle,
                    // as with a zero one): native normalizes a zero quaternion
                    // axis. Its observed transform is zero velocity (unless
                    // the optional BAM refresh then faults).
                    shot.velocity={0,0,0};
                } else {
                    for(float& value:axis)value=float(double(value)/crossLength);
                    const float sine=detmath::sin(turnRadians);
                    const float cosine=detmath::cos(turnRadians);
                    const std::array<float,3> cross={axis[1]*current[2]-axis[2]*current[1],
                        axis[2]*current[0]-axis[0]*current[2],
                        axis[0]*current[1]-axis[1]*current[0]};
                    for(unsigned a=0;a<3;++a) {
                        const float rotated=(current[a]*cosine)+(cross[a]*sine);
                        shot.velocity[a]=detail::retailGuidedFixed(rotated);
                    }
                }
            }
        }
    }
    // 0x52e23d: a modelled shot (+0x93) rebuilds yaw/pitch from its velocity
    // on EVERY update, not only after a turn: with no target, a dead or dying
    // target, or a zero turn angle the steering above is skipped but the
    // refresh still runs, so the spin never accumulates on yaw/pitch.
    if(updateAngles) {
        std::array<float,3> velocity{};
        for(unsigned a=0;a<3;++a)velocity[a]=detail::retailGuidedFloat(shot.velocity[a]);
        const float length=detail::retailGuidedLength(velocity);
        shot.angles[0]=0; // 0x52e28f clears roll before the base substep spin.
        shot.angles[1]=detail::retailGuidedYaw(velocity);
        shot.angles[2]=detail::retailGuidedPitch(velocity,length);
    }

    for(uint32_t step=0;step<substeps;++step) {
        for(unsigned axis=0;axis<3;++axis)shot.angles[axis]=uint16_t(shot.angles[axis]+spin[axis]);
        for(unsigned axis=0;axis<3;++axis)
            shot.position[axis]=std::bit_cast<int32_t>(uint32_t(shot.position[axis])+uint32_t(shot.velocity[axis]));
        if(collision(shot.position,shot.velocity[1])==2)return true;
    }
    return false;
}

} // namespace tak::sim
