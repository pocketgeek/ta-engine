#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tak::video {
// A hardware (OS) cursor is not part of the rendered frame, so a capture of the
// frame never contains it. The stream worker stamps it back in instead: the
// client passes the picture the OS is showing and where, and the worker draws it
// into each frame after scaling, at the frame's own resolution. This costs the
// render thread nothing beyond handing over a shared, immutable image.

// One cursor picture: straight-alpha RGBA, w*h*4 bytes, hotspot in its own pixels.
struct CursorImage {
    int w = 0, h = 0, hotX = 0, hotY = 0;
    std::vector<uint8_t> rgba;
};

// The pointer to show in one captured frame. `x`,`y` is the hotspot position in
// the REFERENCE space: the refW x refH renderer output the frame was captured
// from (a GPU-shrunk capture still refers to the full window). `scale` is
// reference pixels per image pixel. No image (the default) shows no cursor:
// software cursors are already drawn into the frame, and a pointer outside the
// window shows nothing.
struct StreamCursor {
    std::shared_ptr<const CursorImage> image;
    float x = 0, y = 0, scale = 1;
    int refW = 0, refH = 0;
    bool visible() const {
        return image && image->w > 0 && image->h > 0 &&
               image->rgba.size() >= size_t(image->w) * size_t(image->h) * 4 &&
               refW > 0 && refH > 0 && scale > 0;
    }
};

// Alpha-blends a StreamCursor into a picture. The picture is the whole reference
// area as it appears in the output, picW x picH packed RGBA at `pitch` bytes per
// row (for a letterboxed stream: the inner picture, not the black bars). The
// cursor is resampled to the picture's scale (area average when shrinking,
// bilinear when enlarging, in premultiplied alpha) and clipped at the picture's
// edges. The resampled image is cached while the image and its size stay the
// same, so a resting cursor costs one small blend per frame.
class CursorStamper {
public:
    // Returns true when any pixel was drawn.
    bool stamp(uint8_t* picture, ptrdiff_t pitch, int picW, int picH, const StreamCursor& cursor);

private:
    const CursorImage* cachedImage_ = nullptr;
    std::shared_ptr<const CursorImage> keep_;   // the cache is only valid while this lives
    int cachedW_ = 0, cachedH_ = 0;
    std::vector<uint8_t> premul_;   // cachedW_ x cachedH_ premultiplied RGBA
};
}
