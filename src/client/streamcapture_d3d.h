#pragma once
#include <SDL.h>
#include <memory>
#include "video/stream.h"

namespace tak {
// Stream capture for SDL's Windows Direct3D renderers. SDL_RenderReadPixels
// copies the whole back buffer and waits for the GPU on the render thread: 31 ms
// a frame at 7680x2160 under Direct3D 9 (240 FPS fell to 31), 13.6 ms under
// Direct3D 11. Here the GPU shrinks the frame toward the stream size first, and
// Direct3D 11 also reads back asynchronously, as the OpenGL capture does.
// Elsewhere (and for other renderers) get() returns -1: use the portable path.
class D3DCapture {
public:
    D3DCapture();
    ~D3DCapture();
    D3DCapture(const D3DCapture&) = delete;
    D3DCapture& operator=(const D3DCapture&) = delete;
    // -1: not handled here; 0: nothing delivered this time; 1: a frame went to `stream`.
    // outW/outH: the stream's size (0 = keep the window size). `cursor`: the hardware
    // cursor beside this capture (video/streamcursor.h); a frame delivered later (the
    // Direct3D 11 readback ring) carries the cursor it was captured with.
    int get(SDL_Renderer* ren, int w, int h, int outW, int outH, video::Stream& stream,
            const video::StreamCursor& cursor);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
