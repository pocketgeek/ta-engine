#pragma once
// EVEN OUT SOUND LEVELS (Options; ours, not retail). Retail plays every on-screen
// positional sound at one volume (positional routine 50a7d0: 127 in view, 64 out of
// it), so a file's own mastering decides how loud it is. The expansion's 22 kHz
// Creon cries were mastered 4-12 dB under the sounds they play among (the
// Automaton's loudest 200 ms -20 dBFS, a sword swish -17, a swoosh -12, a cannon
// -8) and were lost in a fight. With the option on, each effect is measured once
// as it loads and a quiet one is raised toward kEvenTarget: by at most kEvenMaxBoost,
// never past kEvenPeak at its peak, and a louder one is left alone.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tak {

inline constexpr float kEvenTargetDb = -15.0f;   // loudest 200 ms, dBFS
inline constexpr float kEvenMaxBoostDb = 10.0f;
inline constexpr float kEvenPeak = 0.95f;        // of full scale

// The RMS (0..1 of full scale) of the loudest 200 ms of interleaved S16 PCM, in
// windows stepped by 50 ms; the whole sound when it is shorter.
inline float loudestRms(const int16_t* s, size_t count, int rate, int channels) {
    if (!s || count == 0) return 0.0f;
    const size_t window = std::max<size_t>(1, size_t(std::max(rate, 1)) * size_t(std::max(channels, 1)) / 5);
    const size_t step = std::max<size_t>(1, window / 4);
    double best = 0;
    for (size_t at = 0;; at += step) {
        const size_t end = std::min(count, at + window);
        double sum = 0;
        for (size_t i = at; i < end; ++i) sum += double(s[i]) * double(s[i]);
        best = std::max(best, sum / double(end - at));
        if (end == count) break;
    }
    return float(std::sqrt(best) / 32768.0);
}

// The gain EVEN OUT SOUND LEVELS applies to a sound: 1 for a silent one or one at
// or above the target, else up to the target within the boost and peak caps.
inline float evenOutGain(const int16_t* s, size_t count, int rate, int channels) {
    const float rms = loudestRms(s, count, rate, channels);
    if (rms <= 0.0f) return 1.0f;
    int peak = 0;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(int(s[i])));
    const float target = std::pow(10.0f, kEvenTargetDb / 20.0f);
    float gain = std::min(target / rms, std::pow(10.0f, kEvenMaxBoostDb / 20.0f));
    if (peak > 0) gain = std::min(gain, kEvenPeak * 32768.0f / float(peak));
    return std::max(gain, 1.0f);
}

}  // namespace tak
