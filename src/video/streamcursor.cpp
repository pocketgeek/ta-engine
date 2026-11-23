#include "video/streamcursor.h"

#include <algorithm>
#include <cmath>

namespace tak::video {
namespace {
// Per output coordinate, the source taps and their weights (summing to 1).
struct Taps { std::vector<int> first, count; std::vector<int> index; std::vector<float> weight; };

Taps buildTaps(int src, int dst) {
    Taps t;
    t.first.resize(size_t(dst)); t.count.resize(size_t(dst));
    const double ratio = double(src) / dst;
    for (int o = 0; o < dst; ++o) {
        t.first[size_t(o)] = int(t.index.size());
        if (dst < src) {   // shrink: average the whole source span the pixel covers
            const double a = o * ratio, b = (o + 1) * ratio;
            for (int i = int(std::floor(a)); i < int(std::ceil(b)) && i < src; ++i) {
                const double cover = std::min(b, double(i + 1)) - std::max(a, double(i));
                if (cover <= 0) continue;
                t.index.push_back(i); t.weight.push_back(float(cover / ratio));
            }
        } else {           // enlarge (or 1:1): bilinear between the two nearest centres
            const double s = (o + 0.5) * ratio - 0.5;
            const int i0 = int(std::floor(s));
            const float f = float(s - i0);
            const int lo = std::clamp(i0, 0, src - 1), hi = std::clamp(i0 + 1, 0, src - 1);
            t.index.push_back(lo); t.weight.push_back(1 - f);
            if (f > 0) { t.index.push_back(hi); t.weight.push_back(f); }
        }
        t.count[size_t(o)] = int(t.index.size()) - t.first[size_t(o)];
    }
    return t;
}

// Premultiply, then resample separably to tw x th; the result stays premultiplied,
// so transparent pixels never bleed their colour into the edge.
void resample(const CursorImage& im, int tw, int th, std::vector<uint8_t>& out) {
    const int w = im.w, h = im.h;
    std::vector<float> pm(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float a = im.rgba[i * 4 + 3] / 255.0f;
        pm[i * 4 + 0] = im.rgba[i * 4 + 0] * a;
        pm[i * 4 + 1] = im.rgba[i * 4 + 1] * a;
        pm[i * 4 + 2] = im.rgba[i * 4 + 2] * a;
        pm[i * 4 + 3] = float(im.rgba[i * 4 + 3]);
    }
    const Taps tx = buildTaps(w, tw), ty = buildTaps(h, th);
    std::vector<float> rows(size_t(tw) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < tw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < tx.count[size_t(x)]; ++k) {
                const size_t tap = size_t(tx.first[size_t(x)] + k);
                const float* p = &pm[(size_t(y) * w + size_t(tx.index[tap])) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * tx.weight[tap];
            }
            std::copy(acc, acc + 4, &rows[(size_t(y) * tw + x) * 4]);
        }
    out.assign(size_t(tw) * th * 4, 0);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < ty.count[size_t(y)]; ++k) {
                const size_t tap = size_t(ty.first[size_t(y)] + k);
                const float* p = &rows[(size_t(ty.index[tap]) * tw + x) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * ty.weight[tap];
            }
            uint8_t* d = &out[(size_t(y) * tw + x) * 4];
            const float a = std::clamp(acc[3], 0.0f, 255.0f);
            d[3] = uint8_t(a + 0.5f);
            for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(acc[c], 0.0f, a) + 0.5f);
        }
}
}  // namespace

bool CursorStamper::stamp(uint8_t* picture, ptrdiff_t pitch, int picW, int picH, const StreamCursor& cursor) {
    if (!picture || picW <= 0 || picH <= 0 || !cursor.visible()) return false;
    const CursorImage& im = *cursor.image;
    const double fx = double(picW) / cursor.refW, fy = double(picH) / cursor.refH;
    const int tw = std::max(1, int(std::lround(im.w * cursor.scale * fx)));
    const int th = std::max(1, int(std::lround(im.h * cursor.scale * fy)));
    // The hotspot lands on the pointer, as the OS places it: on the output pixel that
    // contains the pointer pixel's corner (floor, so the window's last pixel stays in).
    const int left = int(std::floor((cursor.x - im.hotX * cursor.scale) * fx));
    const int top = int(std::floor((cursor.y - im.hotY * cursor.scale) * fy));
    const int x0 = std::max(0, left), y0 = std::max(0, top);
    const int x1 = std::min(picW, left + tw), y1 = std::min(picH, top + th);
    if (x0 >= x1 || y0 >= y1) return false;   // wholly outside the picture

    if (cachedImage_ != &im || cachedW_ != tw || cachedH_ != th) {
        resample(im, tw, th, premul_);
        cachedImage_ = &im; keep_ = cursor.image; cachedW_ = tw; cachedH_ = th;
    }
    bool drew = false;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* s = &premul_[(size_t(y - top) * tw + size_t(x0 - left)) * 4];
        uint8_t* d = picture + ptrdiff_t(y) * pitch + ptrdiff_t(x0) * 4;
        for (int x = x0; x < x1; ++x, s += 4, d += 4) {
            const unsigned a = s[3];
            if (!a) continue;
            const unsigned keep = 255 - a;
            for (int c = 0; c < 4; ++c) d[c] = uint8_t(s[c] + (d[c] * keep + 127) / 255);
            drew = true;
        }
    }
    return drew;
}
}
