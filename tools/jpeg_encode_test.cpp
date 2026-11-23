// jpeg_encode_test — tak::jpeg::encode on synthetic pictures: what it writes is a whole
// baseline JPEG of the size asked for, every colour plane at the picture's own size
// (4:4:4), that tak::jpeg::load reads back close to the original -- one-pixel coloured
// lines on a dark ground included, which is what the text of a saved Benchmark results
// screen is. Sizes that are not whole 8x8 blocks work, alpha is ignored, and a buffer of
// the wrong size is refused.

#include "util/jpeg.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

// A dark panel with a pale one-pixel line every 7 columns and a red one every 11 rows.
std::vector<uint8_t> picture(int w, int h) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            const uint8_t ground[3] = {26, 28, 36}, pale[3] = {235, 225, 180}, red[3] = {240, 130, 120};
            const uint8_t* c = y % 11 == 5 ? red : x % 7 == 3 ? pale : ground;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
            p[3] = uint8_t(x * 31 + y * 17);   // must not matter
        }
    return rgba;
}

void roundTrip(int w, int h) {
    const std::string size = std::to_string(w) + "x" + std::to_string(h);
    const std::vector<uint8_t> rgba = picture(w, h);
    const std::vector<uint8_t> jpg = tak::jpeg::encode(w, h, rgba, 92);
    check(jpg.size() > 4 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[jpg.size() - 2] == 0xFF && jpg.back() == 0xD9,
          size + ": must start with SOI and end with EOI");
    // The frame header: baseline (SOF0), 8 bits, this size, three planes each sampled 1x1.
    bool frame = false;
    for (size_t at = 2; at + 4 <= jpg.size() && jpg[at] == 0xFF && jpg[at + 1] != 0xDA;) {
        const size_t length = size_t(jpg[at + 2]) << 8 | jpg[at + 3];
        if (jpg[at + 1] == 0xC0) {
            check(at + 19 <= jpg.size(), size + ": short SOF");
            const int fh = jpg[at + 5] << 8 | jpg[at + 6], fw = jpg[at + 7] << 8 | jpg[at + 8];
            check(jpg[at + 4] == 8 && fw == w && fh == h && jpg[at + 9] == 3, size + ": SOF must give 8 bits, the size, 3 planes");
            for (int c = 0; c < 3; ++c) check(jpg[at + 11 + 3 * c] == 0x11, size + ": every plane must be sampled 1x1 (4:4:4)");
            frame = true;
        }
        check(jpg[at + 1] != 0xC1 && jpg[at + 1] != 0xC2, size + ": must be a baseline JPEG");
        at += 2 + length;
    }
    check(frame, size + ": no SOF0 segment");

    const tak::jpeg::Image back = tak::jpeg::load(jpg);
    check(back.width == w && back.height == h, size + ": decodes to another size");
    long total = 0;
    int worst = 0;
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (i % 4 == 3) { check(back.rgba[i] == 255, size + ": decoded alpha"); continue; }
        const int d = std::abs(int(back.rgba[i]) - int(rgba[i]));
        total += d;
        worst = std::max(worst, d);
    }
    const double mean = double(total) / double(size_t(w) * h * 3);
    std::cout << size << ": " << jpg.size() << " bytes, mean error " << mean << ", worst " << worst << "\n";
    check(mean < 6.0 && worst < 64, size + ": too far from the original (mean " + std::to_string(mean) + ", worst " +
                                        std::to_string(worst) + ")");
}

}  // namespace

int main() {
    try {
        for (const auto& [w, h] : {std::pair{1, 1}, {8, 8}, {37, 23}, {640, 2}, {3, 480}, {321, 243}}) roundTrip(w, h);
        // The same picture with other alpha is the same file.
        std::vector<uint8_t> a = picture(37, 23), b = a;
        for (size_t i = 3; i < b.size(); i += 4) b[i] = 255;
        check(tak::jpeg::encode(37, 23, a, 92) == tak::jpeg::encode(37, 23, b, 92), "alpha must be ignored");
        check(tak::jpeg::encode(37, 23, a, 60).size() < tak::jpeg::encode(37, 23, a, 92).size(), "quality must matter");
        for (const auto& [w, h, bytes] : {std::tuple{37, 23, size_t(37 * 23 * 3)}, {0, 23, size_t(0)}, {37, -1, size_t(37 * 4)},
                                          {70000, 1, size_t(70000 * 4)}}) {
            bool refused = false;
            try { tak::jpeg::encode(w, h, std::vector<uint8_t>(bytes), 92); } catch (const std::runtime_error&) { refused = true; }
            check(refused, std::to_string(w) + "x" + std::to_string(h) + " with " + std::to_string(bytes) + " bytes must be refused");
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "PASS: jpeg::encode writes whole 4:4:4 baseline JPEGs that read back\n";
    return 0;
}
