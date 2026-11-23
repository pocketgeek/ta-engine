#pragma once

// Speaker layouts shared by SoundBank (in game), MenuMusic (front end) and the
// stream's stereo fold (video/stream.cpp): which speaker each interleaved channel
// feeds, and the matrices that carry a source of one layout onto an output of
// another. Channel order is SDL's (and WAVE's):
//   1 mono | 2 FL FR | 3 FL FR LFE | 4 FL FR BL BR | 5 FL FR LFE BL BR
//   6 FL FR FC LFE BL BR | 7 FL FR FC LFE BC SL SR | 8 FL FR FC LFE BL BR SL SR

#include <algorithm>
#include <array>

namespace tak::audio {

// The output mix rate (retail's Miles ran at 22050; mods may ship 44.1 kHz).
inline constexpr int kMixRate = 44100;

enum class Speaker { M, FL, FR, FC, LFE, BL, BR, SL, SR, BC, None };

inline std::array<Speaker, 8> speakers(int channels) {
    using S = Speaker;
    switch (channels) {
        case 1: return {S::M, S::None, S::None, S::None, S::None, S::None, S::None, S::None};
        case 2: return {S::FL, S::FR, S::None, S::None, S::None, S::None, S::None, S::None};
        case 3: return {S::FL, S::FR, S::LFE, S::None, S::None, S::None, S::None, S::None};
        case 4: return {S::FL, S::FR, S::BL, S::BR, S::None, S::None, S::None, S::None};
        case 5: return {S::FL, S::FR, S::LFE, S::BL, S::BR, S::None, S::None, S::None};
        case 6: return {S::FL, S::FR, S::FC, S::LFE, S::BL, S::BR, S::None, S::None};
        case 7: return {S::FL, S::FR, S::FC, S::LFE, S::BC, S::SL, S::SR, S::None};
        default: return {S::FL, S::FR, S::FC, S::LFE, S::BL, S::BR, S::SL, S::SR};
    }
}
inline int speakerIndex(int channels, Speaker s) {
    const auto roles = speakers(channels);
    for (int i = 0; i < std::clamp(channels, 1, 8); ++i)
        if (roles[size_t(i)] == s) return i;
    return -1;
}
inline bool hasLfe(int channels) { return speakerIndex(channels, Speaker::LFE) >= 0; }

// m[out][in]: output channel `out` = sum of m[out][in] * source channel `in`.
using Matrix = std::array<std::array<float, 8>, 8>;

// A source of `in` channels onto an `out`-channel output, channel for channel:
// every speaker the output has keeps its own feed, and only what it lacks folds
// down, ITU-style at -3 dB -- the centre into FL+FR, a back or side speaker into
// its side-to-side twin (BL<->SL; at unity when the source leaves that twin
// empty) or else the front speaker on its side, the back centre into the rear
// pair. A 6.1 output's back centre gets no feed from a BL/BR source (no upmix). The LFE is dropped where the output has none (the
// usual downmix rule: its content is in the mains already). A mono output takes
// the stereo fold's (L+R)/2. Each output row is then scaled to sum to at most 1,
// as the stream's downmix weights it, so a dense fold cannot clip.
inline Matrix downmixMatrix(int in, int out) {
    Matrix m{};
    in = std::clamp(in, 1, 8); out = std::clamp(out, 1, 8);
    if (out == 1) {
        const Matrix two = downmixMatrix(in, 2);
        for (int k = 0; k < in; ++k)
            m[0][size_t(k)] = in == 1 ? 1.f : 0.5f * (two[0][size_t(k)] + two[1][size_t(k)]);
        return m;
    }
    using S = Speaker;
    constexpr float h = 0.70710678f;
    const auto roles = speakers(in);
    // A twin the source has no channel for is the surround's own speaker (5.1 on
    // 6.1: BL/BR play on SL/SR) and takes it at unity; only a shared one is -3 dB.
    auto twin = [&](S target) { return speakerIndex(in, target) >= 0 ? h : 1.f; };
    for (int k = 0; k < in; ++k) {
        const S s = roles[size_t(k)];
        auto to = [&](S target, float g) {
            const int o = speakerIndex(out, target);
            if (o < 0) return false;
            m[size_t(o)][size_t(k)] += g;
            return true;
        };
        if (s == S::M) { to(S::FL, h); to(S::FR, h); continue; }
        if (to(s, 1.f)) continue;
        switch (s) {
            case S::FC: to(S::FL, h); to(S::FR, h); break;
            case S::BL: if (!to(S::SL, twin(S::SL))) to(S::FL, h); break;
            case S::BR: if (!to(S::SR, twin(S::SR))) to(S::FR, h); break;
            case S::SL: if (!to(S::BL, twin(S::BL))) to(S::FL, h); break;
            case S::SR: if (!to(S::BR, twin(S::BR))) to(S::FR, h); break;
            case S::BC:
                if (speakerIndex(out, S::BL) >= 0) { to(S::BL, h); to(S::BR, h); }
                else if (speakerIndex(out, S::SL) >= 0) { to(S::SL, h); to(S::SR, h); }
                else { to(S::FL, 0.5f); to(S::FR, 0.5f); }
                break;
            default: break;   // LFE without a sub: dropped
        }
    }
    for (int o = 0; o < out; ++o) {
        float sum = 0;
        for (int k = 0; k < in; ++k) sum += m[size_t(o)][size_t(k)];
        if (sum > 1.f) for (int k = 0; k < in; ++k) m[size_t(o)][size_t(k)] /= sum;
    }
    return m;
}

// Music onto the output. A mono or stereo track keeps the game's music bed: L on
// the left-side speakers and R on the right, halved on surround rigs so more
// speakers don't make it louder, a quarter of each in the centre, never the LFE
// (SoundBank feeds the sub from the mains instead); on 2.1/4.1/6.1 the front pair
// only, as SoundBank's channelGains does. A track with more than two channels is
// kept as authored and maps channel for channel (downmixMatrix).
inline Matrix musicMatrix(int in, int out) {
    in = std::clamp(in, 1, 8); out = std::clamp(out, 1, 8);
    if (in > 2) return downmixMatrix(in, out);
    Matrix m{};
    const size_t r = in == 2 ? 1 : 0;   // source channel carrying R (mono: the same one)
    switch (out) {
        case 1: m[0][0] = in == 2 ? 0.5f : 1.f; if (in == 2) m[0][1] = 0.5f; break;
        case 4:
            m[0][0] = m[2][0] = 0.5f; m[1][r] = m[3][r] = 0.5f; break;
        case 6: case 8:
            m[0][0] = m[4][0] = 0.5f; m[1][r] = m[5][r] = 0.5f;
            m[2][0] += 0.25f; m[2][r] += 0.25f;
            if (out == 8) { m[6][0] = 0.5f; m[7][r] = 0.5f; }
            break;
        default: m[0][0] = 1.f; m[1][r] = 1.f; break;   // stereo, 2.1, 4.1, 6.1
    }
    return m;
}

}  // namespace tak::audio
