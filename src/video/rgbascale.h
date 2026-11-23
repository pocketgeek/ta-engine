#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace tak::video {
// Resizes packed 8-bit, four-channel pixels for the stream encoder. Shrinking
// averages each output pixel's whole source area (a box at exact 2:1, as MAX
// 3840 sends a 7680-wide window); enlarging is bilinear. Integer fixed point,
// separable, with output rows split over a few helper threads. It replaces
// swscale's single-threaded RGBA bilinear, which needed ~50 ms per 7680x2160
// frame and capped a 60 FPS stream at ~18 FPS. All channels are filtered the
// same way, so RGBA and BGRA both pass through unchanged.
class RgbaScaler {
public:
    // helpers < 0: pick from the machine (up to four helper threads).
    explicit RgbaScaler(int helpers = -1);
    ~RgbaScaler();
    RgbaScaler(const RgbaScaler&) = delete;
    RgbaScaler& operator=(const RgbaScaler&) = delete;
    void scale(const uint8_t* src, int sw, int sh, ptrdiff_t spitch,
               uint8_t* dst, int dw, int dh, ptrdiff_t dpitch);
    int helpers() const { return int(threads_.size()); }

private:
    struct Axis { // per output coordinate: first source index, tap count, Q14 weights
        int src = 0, dst = 0, maxTaps = 0;
        std::vector<int> first, count;
        std::vector<uint16_t> weight; // dst * maxTaps
        void build(int srcN, int dstN);
    };
    struct Job {
        const uint8_t* src; ptrdiff_t spitch; uint8_t* dst; ptrdiff_t dpitch;
    };
    void band(int index, int bands);
    void helper(int index);
    Axis x_, y_;
    Job job_{};
    std::vector<std::vector<uint32_t>> acc_;
    std::vector<std::vector<uint16_t>> row_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    uint64_t generation_ = 0;
    int pending_ = 0, bands_ = 1;
    bool quit_ = false;
};
}
