// The stream encoder's RGBA scaler (src/video/rgbascale.h): exact 2:1 boxes,
// constant colours kept exactly, area/bilinear results within one step of a
// floating-point reference, and identical output with and without helper
// threads. Prints (never asserts) the 7680x2160 -> 3840x1080 time.
#include "video/rgbascale.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int failures = 0;
void require(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; }
}
std::vector<uint8_t> pattern(int w, int h, unsigned seed) {
    std::vector<uint8_t> p(size_t(w) * h * 4);
    for (size_t i = 0; i < p.size(); ++i) { seed = seed * 1103515245u + 12345u; p[i] = uint8_t(seed >> 23); }
    return p;
}
// Floating-point reference of the same filters: area when shrinking, bilinear when enlarging.
std::vector<double> taps(int s, int d, int i, int& first) {
    std::vector<double> w;
    if (s == d) { first = i; return {1.0}; }
    if (s > d) {
        const double r = double(s) / d, lo = i * r, hi = lo + r;
        first = int(std::floor(lo));
        for (int j = first; j < s && j < hi; ++j) w.push_back((std::min(hi, j + 1.0) - std::max(lo, double(j))) / r);
        return w;
    }
    double c = (i + 0.5) * s / d - 0.5;
    if (c <= 0) { first = 0; return {1.0}; }
    first = int(c);
    if (first >= s - 1) { first = s - 1; return {1.0}; }
    return {1.0 - (c - first), c - first};
}
int maxError(const std::vector<uint8_t>& src, int sw, int sh, const std::vector<uint8_t>& out, int dw, int dh) {
    int worst = 0;
    for (int y = 0; y < dh; ++y) {
        int fy; auto wy = taps(sh, dh, y, fy);
        for (int x = 0; x < dw; ++x) {
            int fx; auto wx = taps(sw, dw, x, fx);
            for (int c = 0; c < 4; ++c) {
                double v = 0;
                for (size_t a = 0; a < wy.size(); ++a)
                    for (size_t b = 0; b < wx.size(); ++b)
                        v += wy[a] * wx[b] * src[(size_t(fy + a) * sw + fx + b) * 4 + c];
                worst = std::max(worst, std::abs(int(std::lround(v)) - out[(size_t(y) * dw + x) * 4 + c]));
            }
        }
    }
    return worst;
}
}

int main() {
    tak::video::RgbaScaler threaded(3), single(0);
    // Exact 2:1 box: every output channel is the rounded mean of its 2x2 block.
    {
        const int sw = 1280, sh = 720, dw = 640, dh = 360;
        auto src = pattern(sw, sh, 1);
        std::vector<uint8_t> out(size_t(dw) * dh * 4);
        threaded.scale(src.data(), sw, sh, sw * 4, out.data(), dw, dh, dw * 4);
        int worst = 0;
        for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) for (int c = 0; c < 4; ++c) {
            auto at = [&](int yy, int xx) { return int(src[(size_t(yy) * sw + xx) * 4 + c]); };
            const double mean = (at(2*y, 2*x) + at(2*y, 2*x+1) + at(2*y+1, 2*x) + at(2*y+1, 2*x+1)) / 4.0;
            worst = std::max(worst, int(std::lround(std::abs(mean - out[(size_t(y) * dw + x) * 4 + c]) * 4)));
        }
        require(worst <= 2, "2:1 box within half a step of the exact mean (" + std::to_string(worst) + "/4)");
    }
    // Shapes: shrink by integer and fractional ratios, enlarge, one axis only, tiny.
    const int shapes[][4] = {{7680, 2160, 3840, 1080}, {3440, 1440, 1920, 802}, {3440, 1440, 1280, 534},
                             {1920, 1080, 2560, 1440}, {1280, 720, 1920, 1080}, {1000, 600, 1000, 300},
                             {640, 480, 642, 480}, {5, 3, 2, 2}, {2, 2, 7, 5}, {1582, 934, 1280, 756}};
    for (const auto& s : shapes) {
        const std::string name = std::to_string(s[0]) + "x" + std::to_string(s[1]) + "->" +
                                 std::to_string(s[2]) + "x" + std::to_string(s[3]);
        auto src = pattern(s[0], s[1], unsigned(s[0] * 7 + s[3]));
        std::vector<uint8_t> a(size_t(s[2]) * s[3] * 4), b(a.size());
        threaded.scale(src.data(), s[0], s[1], s[0] * 4, a.data(), s[2], s[3], s[2] * 4);
        single.scale(src.data(), s[0], s[1], s[0] * 4, b.data(), s[2], s[3], s[2] * 4);
        require(a == b, name + ": threaded output equals single-threaded");
        if (int64_t(s[2]) * s[3] <= 1300000) {
            const int e = maxError(src, s[0], s[1], a, s[2], s[3]);
            require(e <= 1, name + ": within one step of the reference (max " + std::to_string(e) + ")");
        }
        std::vector<uint8_t> flat(size_t(s[0]) * s[1] * 4);
        for (size_t i = 0; i < flat.size(); i += 4) { flat[i] = 0; flat[i+1] = 37; flat[i+2] = 200; flat[i+3] = 255; }
        threaded.scale(flat.data(), s[0], s[1], s[0] * 4, a.data(), s[2], s[3], s[2] * 4);
        bool same = true;
        for (size_t i = 0; i < a.size(); i += 4) same = same && a[i] == 0 && a[i+1] == 37 && a[i+2] == 200 && a[i+3] == 255;
        require(same, name + ": a constant colour stays exact");
    }
    // Strides: a padded source and a destination written in place (letterbox offset).
    {
        const int sw = 300, sh = 200, dw = 150, dh = 100, spitch = sw * 4 + 64, dpitch = 1024;
        std::vector<uint8_t> src(size_t(spitch) * sh, 9), dst(size_t(dpitch) * (dh + 2), 77);
        auto packed = pattern(sw, sh, 5);
        for (int y = 0; y < sh; ++y) std::copy_n(&packed[size_t(y) * sw * 4], sw * 4, &src[size_t(y) * spitch]);
        std::vector<uint8_t> want(size_t(dw) * dh * 4);
        single.scale(packed.data(), sw, sh, sw * 4, want.data(), dw, dh, dw * 4);
        threaded.scale(src.data(), sw, sh, spitch, dst.data() + dpitch + 40, dw, dh, dpitch);
        bool ok = true;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw * 4; ++x) ok = ok && dst[size_t(y + 1) * dpitch + 40 + x] == want[size_t(y) * dw * 4 + x];
        ok = ok && dst[0] == 77 && dst[dpitch + 39] == 77 && dst[dpitch + 40 + dw * 4] == 77 && dst[size_t(dh + 1) * dpitch + 40] == 77;
        require(ok, "pitches honoured, nothing outside the target rectangle written");
    }
    {
        tak::video::RgbaScaler automatic;
        auto src = pattern(7680, 2160, 3);
        std::vector<uint8_t> out(size_t(3840) * 1080 * 4);
        automatic.scale(src.data(), 7680, 2160, 7680 * 4, out.data(), 3840, 1080, 3840 * 4);
        const auto t = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) automatic.scale(src.data(), 7680, 2160, 7680 * 4, out.data(), 3840, 1080, 3840 * 4);
        std::printf("7680x2160 -> 3840x1080: %.2f ms per frame with %d helper threads\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() / 10,
                    automatic.helpers());
    }
    if (failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::printf("rgbascale_test: all checks passed\n");
    return 0;
}
