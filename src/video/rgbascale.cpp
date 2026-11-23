#include "video/rgbascale.h"
#include <algorithm>
#include <cstring>

namespace tak::video {
namespace {
constexpr int kOne = 1 << 14; // Q14 filter weights
}
// Area weights when shrinking (each output pixel averages exactly the source
// span it covers), bilinear when enlarging. Weights of a tap list sum to kOne.
void RgbaScaler::Axis::build(int srcN, int dstN) {
    if (src == srcN && dst == dstN) return;
    src = srcN; dst = dstN;
    first.assign(dstN, 0); count.assign(dstN, 0);
    maxTaps = srcN > dstN ? (srcN + dstN - 1) / dstN + 1 : 2;
    weight.assign(size_t(dstN) * maxTaps, 0);
    for (int i = 0; i < dstN; ++i) {
        uint16_t* w = &weight[size_t(i) * maxTaps];
        if (srcN == dstN) { first[i] = i; count[i] = 1; w[0] = kOne; continue; }
        if (srcN > dstN) {
            // In units of 1/(srcN*dstN): source j spans [j*dstN, (j+1)*dstN),
            // output i spans [i*srcN, (i+1)*srcN).
            const int64_t lo = int64_t(i) * srcN, hi = lo + srcN;
            const int j0 = int(lo / dstN), j1 = int(std::min<int64_t>(srcN, (hi + dstN - 1) / dstN));
            first[i] = j0; count[i] = j1 - j0;
            int sum = 0, big = 0;
            for (int j = j0; j < j1; ++j) {
                const int64_t overlap = std::min<int64_t>(hi, int64_t(j + 1) * dstN) - std::max<int64_t>(lo, int64_t(j) * dstN);
                const int k = j - j0;
                w[k] = uint16_t((overlap * kOne + srcN / 2) / srcN);
                sum += w[k]; if (w[k] > w[big]) big = k;
            }
            w[big] = uint16_t(w[big] + kOne - sum);
        } else {
            // Pixel centres: source position (i + 0.5) * srcN / dstN - 0.5.
            const int64_t num = int64_t(2 * i + 1) * srcN - dstN, den = int64_t(2) * dstN;
            int j = num <= 0 ? 0 : int(num / den);
            int f = num <= 0 ? 0 : int((num - j * den) * kOne / den);
            if (j >= srcN - 1) { j = srcN - 1; f = 0; }
            first[i] = j;
            if (f == 0) { count[i] = 1; w[0] = kOne; }
            else { count[i] = 2; w[0] = uint16_t(kOne - f); w[1] = uint16_t(f); }
        }
    }
}

RgbaScaler::RgbaScaler(int helpers) {
    if (helpers < 0) {
        const unsigned cores = std::thread::hardware_concurrency();
        helpers = cores >= 16 ? 4 : cores >= 8 ? 2 : cores >= 4 ? 1 : 0;
    }
    acc_.resize(helpers + 1); row_.resize(helpers + 1);
    try {
        for (int i = 0; i < helpers; ++i) threads_.emplace_back([this, i] { helper(i + 1); });
    } catch (...) {} // fewer helpers; the caller's band still covers the work
}
RgbaScaler::~RgbaScaler() {
    { std::lock_guard lock(mutex_); quit_ = true; }
    start_.notify_all();
    for (auto& t : threads_) t.join();
}
void RgbaScaler::helper(int index) {
    uint64_t seen = 0;
    for (;;) {
        int bands;
        { std::unique_lock lock(mutex_);
          start_.wait(lock, [&] { return quit_ || generation_ != seen; });
          if (quit_) return;
          seen = generation_; bands = bands_; }
        if (index < bands) band(index, bands);
        { std::lock_guard lock(mutex_); if (--pending_ == 0) done_.notify_one(); }
    }
}
void RgbaScaler::band(int index, int bands) {
    const int dh = y_.dst, sw = x_.src, dw = x_.dst;
    const int y0 = int(int64_t(dh) * index / bands), y1 = int(int64_t(dh) * (index + 1) / bands);
    auto& acc = acc_[index]; auto& row = row_[index];
    const size_t n = size_t(sw) * 4;
    if (x_.dst * 2 == x_.src && y_.dst * 2 == y_.src) {
        // Exact 2:1 both ways: the same rounded 2x2 mean the general path computes,
        // (sum + 2) >> 2, in one pass over the two source rows.
        for (int y = y0; y < y1; ++y) {
            const uint8_t* a = job_.src + ptrdiff_t(2 * y) * job_.spitch;
            const uint8_t* b = a + job_.spitch;
            uint8_t* out = job_.dst + ptrdiff_t(y) * job_.dpitch;
            for (int x = 0; x < dw; ++x)
                for (int c = 0; c < 4; ++c)
                    out[x*4+c] = uint8_t((a[x*8+c] + a[x*8+4+c] + b[x*8+c] + b[x*8+4+c] + 2) >> 2);
        }
        return;
    }
    if (row.size() < n) { row.resize(n); acc.resize(n); }
    for (int y = y0; y < y1; ++y) {
        // Vertical: the output row's source rows into one Q6 row of source width.
        const uint16_t* wy = &y_.weight[size_t(y) * y_.maxTaps];
        const uint8_t* s0 = job_.src + ptrdiff_t(y_.first[y]) * job_.spitch;
        uint16_t* r = row.data();
        if (y_.count[y] == 1) {
            for (size_t i = 0; i < n; ++i) r[i] = uint16_t(s0[i] << 6);
        } else if (y_.count[y] == 2) {
            const uint8_t* s1 = s0 + job_.spitch;
            const uint32_t w0 = wy[0], w1 = wy[1];
            for (size_t i = 0; i < n; ++i) r[i] = uint16_t((w0 * s0[i] + w1 * s1[i] + 128) >> 8);
        } else {
            uint32_t* a = acc.data();
            for (size_t i = 0; i < n; ++i) a[i] = wy[0] * uint32_t(s0[i]);
            for (int k = 1; k < y_.count[y]; ++k) {
                const uint8_t* s = s0 + ptrdiff_t(k) * job_.spitch;
                const uint32_t w = wy[k];
                for (size_t i = 0; i < n; ++i) a[i] += w * s[i];
            }
            for (size_t i = 0; i < n; ++i) r[i] = uint16_t((a[i] + 128) >> 8);
        }
        // Horizontal: Q6 x Q14 sums fit 32 bits (16320 * 16384 < 2^28).
        uint8_t* out = job_.dst + ptrdiff_t(y) * job_.dpitch;
        for (int x = 0; x < dw; ++x) {
            const uint16_t* wx = &x_.weight[size_t(x) * x_.maxTaps];
            const uint16_t* p = r + size_t(x_.first[x]) * 4;
            uint32_t c0 = 1u << 19, c1 = c0, c2 = c0, c3 = c0;
            for (int k = 0, taps = x_.count[x]; k < taps; ++k, p += 4) {
                const uint32_t w = wx[k];
                c0 += w * p[0]; c1 += w * p[1]; c2 += w * p[2]; c3 += w * p[3];
            }
            out[x*4] = uint8_t(c0 >> 20); out[x*4+1] = uint8_t(c1 >> 20);
            out[x*4+2] = uint8_t(c2 >> 20); out[x*4+3] = uint8_t(c3 >> 20);
        }
    }
}
void RgbaScaler::scale(const uint8_t* src, int sw, int sh, ptrdiff_t spitch,
                       uint8_t* dst, int dw, int dh, ptrdiff_t dpitch) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) {
        for (int y = 0; y < sh; ++y) std::memcpy(dst + ptrdiff_t(y) * dpitch, src + ptrdiff_t(y) * spitch, size_t(sw) * 4);
        return;
    }
    x_.build(sw, dw); y_.build(sh, dh);
    job_ = {src, spitch, dst, dpitch};
    // Small frames are not worth waking anyone for.
    const int bands = int64_t(sw) * sh + int64_t(dw) * dh < 1000000 || dh < 8 ? 1 : int(threads_.size()) + 1;
    if (bands > 1) {
        { std::lock_guard lock(mutex_); bands_ = bands; pending_ = int(threads_.size()); ++generation_; }
        start_.notify_all();
    }
    band(0, bands);
    if (bands > 1) {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0; });
    }
}
}
