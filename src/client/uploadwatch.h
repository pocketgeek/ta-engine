#pragma once
// The streaming panel's "UPLOAD CAN'T KEEP UP" warning (client/streaming.cpp), for any
// destination. Fed each RATE sample (about one a second) of a LIVE stream: the DROPPED
// count, and how much of the sample the encoder spent waiting for the connection to take
// its packets (StreamStatus::sendMicros). A sample is BEHIND when it waited at least
// half the time, or DROPPED rose while it waited at least a tenth. It warns when three
// of the last five samples are behind, and clears after five that are not. The first
// samples of a connection are ignored while the encoder and the connection settle.
//
// RATE under the set bitrate is not a sign on its own: a hardware encoder that is not
// strictly constant-bitrate sends far less on a still scene. Nor are dropped frames
// while the connection takes everything at once: then the computer, not the upload, is
// what falls behind.
#include <bit>
#include <cstdint>

namespace tak {

class UploadWatch {
public:
    void reset() { *this = {}; }
    // Returns whether the warning is on after this sample.
    bool sample(uint64_t dropped, uint64_t waitedMicros, uint64_t sampleMicros) {
        const bool rose = seen_ && dropped > dropped_;
        dropped_ = dropped; seen_ = true;
        if (settle_ > 0) { --settle_; return warn_; }
        const bool behind = sampleMicros > 0 &&
            (waitedMicros * 2 >= sampleMicros || (rose && waitedMicros * 10 >= sampleMicros));
        behind_ = (behind_ << 1 | behind) & 31u;
        if (std::popcount(behind_) >= 3) warn_ = true;
        else if (!behind_) warn_ = false;
        return warn_;
    }
    bool warning() const { return warn_; }
private:
    unsigned behind_ = 0; // the last five samples, newest in bit 0
    uint64_t dropped_ = 0;
    bool seen_ = false, warn_ = false;
    int settle_ = 3;
};

}
