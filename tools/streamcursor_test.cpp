// Hardware-cursor stamping into stream frames (video/streamcursor.h) and the cursor-path
// decisions (client/cursorpath.h). CPU only: no window, GPU or FFmpeg.
#include "client/cursorpath.h"
#include "video/streamcursor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using tak::video::CursorImage;
using tak::video::CursorStamper;
using tak::video::StreamCursor;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Picture {
    int w, h;
    std::vector<uint8_t> px;
    Picture(int w_, int h_, uint8_t grey = 0) : w(w_), h(h_), px(size_t(w_) * h_ * 4, grey) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
    const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
    bool is(int x, int y, int r, int g, int b, int tol = 0) const {
        const uint8_t* p = at(x, y);
        return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
    }
    // The bounding box of pixels that are no longer `grey`.
    bool changed(int& x0, int& y0, int& x1, int& y1, uint8_t grey = 0) const {
        x0 = w; y0 = h; x1 = -1; y1 = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = at(x, y);
                if (p[0] == grey && p[1] == grey && p[2] == grey) continue;
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
            }
        return x1 >= 0;
    }
};

// An opaque w x h block of one colour, hotspot (hx,hy).
std::shared_ptr<CursorImage> block(int w, int h, int hx, int hy, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    auto im = std::make_shared<CursorImage>();
    im->w = w; im->h = h; im->hotX = hx; im->hotY = hy;
    im->rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        im->rgba[i * 4 + 0] = r; im->rgba[i * 4 + 1] = g; im->rgba[i * 4 + 2] = b; im->rgba[i * 4 + 3] = a;
    }
    return im;
}

StreamCursor at(std::shared_ptr<CursorImage> im, float x, float y, float scale, int refW, int refH) {
    StreamCursor c; c.image = std::move(im); c.x = x; c.y = y; c.scale = scale; c.refW = refW; c.refH = refH;
    return c;
}
}  // namespace

int main() {
    // 1. 1:1: the hotspot lands on the pointer, the sprite keeps its size.
    {
        Picture p(64, 48); CursorStamper s;
        check(s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(block(4, 4, 1, 1, 255, 0, 0), 10, 10, 1, 64, 48)),
              "1:1 stamp drew nothing");
        int x0, y0, x1, y1; p.changed(x0, y0, x1, y1);
        check(x0 == 9 && y0 == 9 && x1 == 12 && y1 == 12, "1:1 stamp misplaced (hotspot not on the pointer)");
        check(p.is(10, 10, 255, 0, 0), "1:1 stamp colour");
    }
    // 2. CURSOR SIZE: scale magnifies the sprite AND its hotspot.
    {
        Picture p(64, 48); CursorStamper s;
        s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(block(4, 4, 1, 1, 0, 255, 0), 20, 20, 2, 64, 48));
        int x0, y0, x1, y1; p.changed(x0, y0, x1, y1);
        check(x0 == 18 && y0 == 18 && x1 == 25 && y1 == 25, "scale 2: wrong rectangle");
    }
    // 3. Capture-to-output scale: a 200x100 window streamed at 100x50 halves both the
    //    position and the sprite (the cursor keeps its share of the picture).
    {
        Picture p(100, 50); CursorStamper s;
        s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(block(16, 16, 0, 0, 0, 0, 255), 100, 50, 1, 200, 100));
        int x0, y0, x1, y1; p.changed(x0, y0, x1, y1);
        check(x0 == 50 && y0 == 25 && x1 == 57 && y1 == 32, "half-size stream: wrong rectangle");
        check(p.is(53, 28, 0, 0, 255), "half-size stream: area-averaged colour");
        // Enlarging (a small window streamed bigger) doubles it, bilinear inside.
        Picture q(400, 200); CursorStamper t;
        t.stamp(q.px.data(), q.w * 4, q.w, q.h, at(block(4, 4, 0, 0, 0, 0, 255), 10, 10, 1, 200, 100));
        q.changed(x0, y0, x1, y1);
        check(x0 == 20 && y0 == 20 && x1 == 27 && y1 == 27, "double-size stream: wrong rectangle");
    }
    // 4. Letterbox: the picture is the inner rectangle of a bigger output frame; the
    //    cursor maps onto it and never draws into the black bars, even at the edge.
    {
        Picture frame(120, 80); CursorStamper s;
        const int ox = 10, oy = 15, pw = 100, ph = 50;   // inner picture
        uint8_t* inner = frame.px.data() + (size_t(oy) * frame.w + ox) * 4;
        // Pointer at the window's bottom-right corner: most of the sprite falls outside.
        s.stamp(inner, frame.w * 4, pw, ph, at(block(8, 8, 0, 0, 255, 255, 255), 199, 99, 1, 200, 100));
        int x0, y0, x1, y1;
        check(frame.changed(x0, y0, x1, y1), "letterbox corner: nothing drawn");
        check(x0 >= ox && y0 >= oy && x1 < ox + pw && y1 < oy + ph, "letterbox: drew into the bars");
        check(x1 == ox + pw - 1 && y1 == oy + ph - 1, "letterbox: corner not reached");
    }
    // 5. Clipping: partly off the top-left, and wholly outside.
    {
        Picture p(32, 32); CursorStamper s;
        check(s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(block(8, 8, 4, 4, 255, 0, 0), 0, 0, 1, 32, 32)),
              "top-left clip drew nothing");
        int x0, y0, x1, y1; p.changed(x0, y0, x1, y1);
        check(x0 == 0 && y0 == 0 && x1 == 3 && y1 == 3, "top-left clip: wrong part kept");
        Picture q(32, 32); CursorStamper t;
        check(!t.stamp(q.px.data(), q.w * 4, q.w, q.h, at(block(8, 8, 0, 0, 255, 0, 0), 40, 40, 1, 32, 32)),
              "outside the picture drew something");
        check(!q.changed(x0, y0, x1, y1), "outside the picture changed pixels");
    }
    // 6. Alpha: half-transparent white over black is mid grey; transparent stays put.
    {
        Picture p(16, 16); CursorStamper s;
        s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(block(4, 4, 0, 0, 255, 255, 255, 128), 4, 4, 1, 16, 16));
        check(p.is(5, 5, 128, 128, 128, 1), "50% alpha blend");
        check(p.at(5, 5)[3] == 255, "opaque frame stays opaque");
        Picture q(16, 16, 77); CursorStamper t;
        check(!t.stamp(q.px.data(), q.w * 4, q.w, q.h, at(block(4, 4, 0, 0, 255, 0, 0, 0), 4, 4, 1, 16, 16)),
              "transparent sprite drew");
        int x0, y0, x1, y1; check(!q.changed(x0, y0, x1, y1, 77), "transparent sprite changed pixels");
        // Transparent pixels beside opaque ones do not darken the edge (premultiplied).
        auto im = block(2, 1, 0, 0, 255, 255, 255);
        im->rgba[4] = im->rgba[5] = im->rgba[6] = 0; im->rgba[7] = 0;   // right pixel clear, black
        Picture e(8, 8, 200); CursorStamper u;
        u.stamp(e.px.data(), e.w * 4, e.w, e.h, at(im, 0, 0, 4, 8, 8));   // 8x4 enlarged
        check(e.is(0, 0, 255, 255, 255), "enlarged opaque side");
        check(e.at(3, 0)[0] >= 230, "edge darkened by a transparent neighbour");
    }
    // 7. Cache: the same image twice reuses the resample; a new image is not stale.
    {
        Picture p(16, 16); CursorStamper s;
        auto red = block(2, 2, 0, 0, 255, 0, 0), green = block(2, 2, 0, 0, 0, 255, 0);
        s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(red, 1, 1, 1, 16, 16));
        s.stamp(p.px.data(), p.w * 4, p.w, p.h, at(green, 8, 8, 1, 16, 16));
        check(p.is(1, 1, 255, 0, 0) && p.is(8, 8, 0, 255, 0), "cache served a stale image");
    }
    // 8. No cursor: a default StreamCursor (software cursor, or the pointer outside the
    //    window) never draws, so a software-drawn cursor is never doubled.
    {
        Picture p(16, 16); CursorStamper s;
        check(!s.stamp(p.px.data(), p.w * 4, p.w, p.h, StreamCursor{}), "empty cursor drew");
        int x0, y0, x1, y1; check(!p.changed(x0, y0, x1, y1), "empty cursor changed pixels");
    }
    // 9. The decisions. Streaming does not change the path; the stamp needs our own
    //    cursor shown by the OS with the pointer over the window.
    {
        check(tak::useHardwareCursor(true, false), "hardware on -> hardware");
        check(!tak::useHardwareCursor(true, true), "refused -> software");
        check(!tak::useHardwareCursor(false, false), "hardware off -> software");
        check(tak::stampHardwareCursor(true, true, true, true), "shown hardware cursor -> stamp");
        check(!tak::stampHardwareCursor(true, false, true, true), "software (OS hidden) -> no stamp");
        check(!tak::stampHardwareCursor(true, true, false, true), "system arrow -> no stamp");
        check(!tak::stampHardwareCursor(true, true, true, false), "pointer outside -> no stamp");
        check(!tak::stampHardwareCursor(false, true, true, true), "no image -> no stamp");
        tak::HardwareCursorStatus st;
        check(tak::hardwareCursorNote(true, st).empty(), "note before any bake");
        st.tried = true; st.w = 64; st.h = 100;
        check(tak::hardwareCursorNote(true, st) == "OS CURSOR 64X100", "note when ok");
        check(tak::hardwareCursorNote(false, st).empty(), "note when the option is off");
        st.failed = true;
        check(tak::hardwareCursorNote(true, st) == "UNAVAILABLE - SOFTWARE", "note when refused");
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("streamcursor: all checks passed\n");
    return 0;
}
