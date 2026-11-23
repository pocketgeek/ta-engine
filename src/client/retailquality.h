#pragma once

#include <bit>
#include <cstdint>

namespace tak {

// 4ec7cb..4ec879: visual refresh decision. This state belongs to the outer
// renderer, whose cadence and visible-unit order must be supplied by its host.
// It is deliberately not called once per simulation tick.
struct RetailVisualQuality {
    int32_t smoothedFps=60;
    float probability=0;

    // Inputs are finite, nonnegative FPS, a positive threshold and a setting
    // in 0..100 (fixed percent) or above 100 (adaptive). Random returns 0..32767.
    template<class Random>
    bool choose(int32_t setting,double framesPerSecond,float threshold,float bias,Random&& random) {
        if (setting<=100) return int64_t(random(0x4ec7ebu))*100<=int64_t(setting)*32767;
        const int32_t measured=int32_t(framesPerSecond);
        if (smoothedFps<measured) ++smoothedFps;
        else if (smoothedFps>measured) --smoothedFps;
        if (double(smoothedFps)>double(threshold)) return true;
        probability=float(double(smoothedFps)/double(threshold)+double(bias)/(double(smoothedFps)+1.0));
        constexpr float scale=std::bit_cast<float>(uint32_t(0x38000100));
        return double(random(0x4ec858u))*double(scale)<=double(probability);
    }
};

} // namespace tak
