#include "util/appicon.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tak::appicon {

namespace {

struct Col { float r, g, b, a; };
constexpr Col hex(uint32_t rgb, float a = 1.0f) {
    return {float((rgb >> 16) & 255) / 255.0f, float((rgb >> 8) & 255) / 255.0f,
            float(rgb & 255) / 255.0f, a};
}

// A floating-point RGBA canvas we composite shapes onto (source-over), then
// box-downsample. All coordinates are in supersampled pixels.
struct Canvas {
    int w;
    std::vector<Col> px;   // straight (non-premultiplied) RGBA
    explicit Canvas(int side) : w(side), px(size_t(side) * side, {0, 0, 0, 0}) {}

    void over(int x, int y, Col s) {
        if (x < 0 || y < 0 || x >= w || y >= w || s.a <= 0) return;
        Col& d = px[size_t(y) * w + x];
        float oa = s.a + d.a * (1 - s.a);
        if (oa <= 1e-6f) { d = {0, 0, 0, 0}; return; }
        d.r = (s.r * s.a + d.r * d.a * (1 - s.a)) / oa;
        d.g = (s.g * s.a + d.g * d.a * (1 - s.a)) / oa;
        d.b = (s.b * s.a + d.b * d.a * (1 - s.a)) / oa;
        d.a = oa;
    }

    void disk(float cx, float cy, float rad, Col c) {
        int x0 = int(cx - rad) - 1, x1 = int(cx + rad) + 1;
        int y0 = int(cy - rad) - 1, y1 = int(cy + rad) + 1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                if (dx * dx + dy * dy <= rad * rad) over(x, y, c);
            }
    }

    // Ring segment (annulus wedge) between angles a0..a1 (radians, 0 = +x, CCW).
    void arc(float cx, float cy, float rIn, float rOut, float a0, float a1, Col c) {
        int x0 = int(cx - rOut) - 1, x1 = int(cx + rOut) + 1;
        int y0 = int(cy - rOut) - 1, y1 = int(cy + rOut) + 1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float d = std::sqrt(dx * dx + dy * dy);
                if (d < rIn || d > rOut) continue;
                float ang = std::atan2(-dy, dx);   // screen y is down
                if (ang < 0) ang += 6.2831853f;
                if (ang >= a0 && ang <= a1) over(x, y, c);
            }
    }

    // Filled convex/simple polygon (even-odd scanline).
    void poly(const std::vector<std::array<float, 2>>& p, Col c) {
        float miny = 1e9f, maxy = -1e9f;
        for (auto& v : p) { miny = std::min(miny, v[1]); maxy = std::max(maxy, v[1]); }
        for (int y = int(miny); y <= int(maxy); ++y) {
            float yc = y + 0.5f;
            std::vector<float> xs;
            for (size_t i = 0, n = p.size(); i < n; ++i) {
                auto& a = p[i];
                auto& b = p[(i + 1) % n];
                if ((a[1] <= yc) != (b[1] <= yc)) {
                    float t = (yc - a[1]) / (b[1] - a[1]);
                    xs.push_back(a[0] + t * (b[0] - a[0]));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2)
                for (int x = int(xs[i]); x <= int(xs[i + 1]); ++x) over(x, y, c);
        }
    }

    void rect(float x0, float y0, float x1, float y1, Col c) {
        poly({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, c);
    }

    // Rounded-rect badge with a vertical top->bottom gradient + border ring.
    void badge(float m, float rad, Col top, Col bot, Col border, float bw) {
        auto inRR = [&](float x, float y, float inset) {
            float lx = m + inset, hy = w - m - inset, r = rad - inset;
            float ax = std::clamp(x, lx + r, w - m - inset - r);
            float ay = std::clamp(y, lx + r, hy - r);
            float dx = x - ax, dy = y - ay;
            return dx * dx + dy * dy <= r * r &&
                   x >= lx && x <= w - m - inset && y >= lx && y <= hy;
        };
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x) {
                float fx = x + 0.5f, fy = y + 0.5f;
                if (inRR(fx, fy, bw)) {
                    float t = (fy - m) / (w - 2 * m);
                    over(x, y, {top.r + (bot.r - top.r) * t, top.g + (bot.g - top.g) * t,
                                top.b + (bot.b - top.b) * t, 1});
                } else if (inRR(fx, fy, 0)) {
                    over(x, y, border);
                }
            }
    }
};

void drawCrown(Canvas& c, float S) {
    const Col gold = hex(0xEAC24C), goldHi = hex(0xFFE9A6), goldSh = hex(0xB6862A);
    const Col ruby = hex(0xE0524F), sapph = hex(0x4C86D6);
    float cx = S / 2;
    float bandY0 = S * 0.60f, bandY1 = S * 0.70f;
    float lx = S * 0.26f, rx = S * 0.74f;
    // Five-point crown silhouette: outer-mid-CENTER-mid-outer, valleys between.
    float topY = S * 0.30f, midY = S * 0.40f, valY = S * 0.52f;
    c.poly({{lx, bandY0}, {lx, S * 0.44f}, {S * 0.34f, valY}, {S * 0.42f, midY},
            {cx, valY}, {S * 0.58f, midY}, {S * 0.66f, valY}, {rx, S * 0.44f},
            {rx, bandY0}}, gold);
    // Re-cap the three tips a touch taller + a highlight down their left edges.
    c.poly({{S * 0.42f, midY}, {cx, topY}, {S * 0.58f, midY}, {cx, valY}}, gold);
    c.poly({{lx, S * 0.44f}, {S * 0.30f, midY}, {S * 0.34f, valY}}, gold);
    c.poly({{rx, S * 0.44f}, {S * 0.70f, midY}, {S * 0.66f, valY}}, gold);
    // Band + rim shading.
    c.rect(lx, bandY0, rx, bandY1, gold);
    c.rect(lx, bandY1 - S * 0.012f, rx, bandY1, goldSh);
    c.rect(lx, bandY0, rx, bandY0 + S * 0.012f, goldHi);
    // Jewels: tips + band.
    c.disk(cx, topY + S * 0.015f, S * 0.028f, sapph);
    c.disk(S * 0.30f, midY + S * 0.01f, S * 0.022f, ruby);
    c.disk(S * 0.70f, midY + S * 0.01f, S * 0.022f, ruby);
    c.disk(cx, (bandY0 + bandY1) / 2, S * 0.03f, ruby);
    c.disk(S * 0.37f, (bandY0 + bandY1) / 2, S * 0.02f, sapph);
    c.disk(S * 0.63f, (bandY0 + bandY1) / 2, S * 0.02f, sapph);
}

void drawCompass(Canvas& c, float S) {
    const Col lite = hex(0xF3EAD0), dark = hex(0x0E403C), gold = hex(0xD9B45A);
    float cx = S / 2, cy = S / 2;
    float R = S * 0.30f, r = S * 0.17f, wBase = S * 0.055f;
    // Thin outer ring.
    c.arc(cx, cy, R + S * 0.02f, R + S * 0.045f, 0, 6.2832f, gold);
    // Eight points as bicolour kites; majors long (N/E/S/W), minors short.
    const float ang[8] = {1.5708f, 0.7854f, 0.0f, -0.7854f, -1.5708f,
                          -2.3562f, 3.1416f, 2.3562f};
    for (int i = 0; i < 8; ++i) {
        bool major = (i % 2) == 0;
        float len = major ? R : r;
        float a = ang[i];
        float tx = cx + std::cos(a) * len, ty = cy - std::sin(a) * len;
        // perpendicular base offsets
        float px = std::cos(a + 1.5708f) * wBase, py = -std::sin(a + 1.5708f) * wBase;
        // two halves: the clockwise face light, the other dark, for a faceted look.
        c.poly({{cx, cy}, {tx, ty}, {cx + px, cy + py}}, lite);
        c.poly({{cx, cy}, {tx, ty}, {cx - px, cy - py}}, dark);
    }
    c.disk(cx, cy, S * 0.05f, gold);
    c.disk(cx, cy, S * 0.022f, dark);
}

void drawKeep(Canvas& c, float S) {
    const Col stone = hex(0xBFC7D0), stoneSh = hex(0x8A96A2), door = hex(0x39424C);
    const Col amber = hex(0xF4B048);
    float bx0 = S * 0.30f, bx1 = S * 0.56f, by0 = S * 0.36f, by1 = S * 0.74f;
    c.rect(bx0, by0, bx1, by1, stone);
    c.rect(bx1 - (bx1 - bx0) * 0.34f, by0, bx1, by1, stoneSh);   // right-side shadow
    // Battlements: three merlons with two gaps.
    float mw = (bx1 - bx0) / 5.0f, mt = by0 - S * 0.05f;
    for (int i = 0; i < 3; ++i)
        c.rect(bx0 + i * 2 * mw, mt, bx0 + i * 2 * mw + mw, by0 + S * 0.005f, i < 2 ? stone : stoneSh);
    // Arched door.
    float dcx = (bx0 + bx1) / 2, dw = (bx1 - bx0) * 0.32f;
    c.rect(dcx - dw / 2, by1 - S * 0.16f, dcx + dw / 2, by1, door);
    c.disk(dcx, by1 - S * 0.16f, dw / 2, door);
    // Broadcast: three amber arcs radiating from the tower top-right.
    float sx = bx1 - S * 0.01f, sy = by0 - S * 0.005f;
    for (int k = 1; k <= 3; ++k)
        c.arc(sx, sy, S * 0.09f * k - S * 0.02f, S * 0.09f * k, -1.15f, 0.35f, amber);
    c.disk(sx, sy, S * 0.028f, amber);
}

} // namespace

std::vector<uint8_t> render(Kind kind, int size) {
    const int SS = 4;
    const int W = size * SS;
    const float S = float(W);
    Canvas c(W);

    // Cohesive badge, per-app palette.
    switch (kind) {
        case Kind::Client:
            c.badge(S * 0.06f, S * 0.22f, hex(0x3C2C74), hex(0x1A1140), hex(0x0C0824), S * 0.02f);
            drawCrown(c, S);
            break;
        case Kind::Cartographer:
            c.badge(S * 0.06f, S * 0.22f, hex(0x1E706A), hex(0x0C3B38), hex(0x05201E), S * 0.02f);
            drawCompass(c, S);
            break;
        case Kind::Server:
            c.badge(S * 0.06f, S * 0.22f, hex(0x5C6672), hex(0x2A3038), hex(0x12161C), S * 0.02f);
            drawKeep(c, S);
            break;
    }

    // Box-downsample SSxSS with premultiplied averaging (clean anti-aliased edges).
    std::vector<uint8_t> out(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float ar = 0, ag = 0, ab = 0, aa = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const Col& p = c.px[size_t((y * SS + sy)) * W + (x * SS + sx)];
                    ar += p.r * p.a; ag += p.g * p.a; ab += p.b * p.a; aa += p.a;
                }
            uint8_t* o = &out[(size_t(y) * size + x) * 4];
            float inv = aa > 1e-6f ? 1.0f / aa : 0.0f;
            auto b = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            o[0] = b(ar * inv); o[1] = b(ag * inv); o[2] = b(ab * inv);
            o[3] = b(aa / (SS * SS));
        }
    return out;
}

} // namespace tak::appicon
