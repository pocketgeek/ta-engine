#pragma once

// The voice chat codec: Opus, 48 kHz mono, 20 ms frames (docs/multiplayer-design.md,
// "Voice chat"). This header and voicecodec.cpp are the only places that know about
// Opus; everything else passes opaque packets. Client only -- the server relays the
// packets without decoding them.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tak::voice {

inline constexpr int kRate = 48000;          // samples per second
inline constexpr int kFrame = 960;           // samples per 20 ms frame
inline constexpr int kFrameMs = 20;
inline constexpr int kBitrate = 24000;       // bits per second (VBR target)
inline constexpr size_t kMaxPacket = 400;    // == net::kVoiceMaxOpus, checked in voice.cpp

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool ok() const { return st_ != nullptr; }
    // One kFrame-sample frame in, one packet out (empty on failure). Never longer than
    // kMaxPacket.
    std::vector<uint8_t> encode(const float* pcm);
    void reset();   // a new transmission starts from a clean predictor state

private:
    void* st_ = nullptr;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    bool ok() const { return st_ != nullptr; }
    // Each fills kFrame samples of `out` and returns true, or returns false (and leaves
    // silence) when the packet is corrupt.
    bool decode(const uint8_t* data, size_t n, float* out);
    // The frame BEFORE `next`, rebuilt from the forward error correction `next` carries
    // (Opus in-band FEC); concealment where it carries none.
    bool decodeFec(const uint8_t* next, size_t n, float* out);
    // Packet loss concealment: a plausible continuation of what was playing.
    bool conceal(float* out);
    void reset();

private:
    void* st_ = nullptr;
};

}  // namespace tak::voice
