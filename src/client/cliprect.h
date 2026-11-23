// cliprect -- SDL_RenderSetClipRect for a rectangle that may cover no output pixel.
//
// SDL2's Metal renderer (src/render/metal/SDL_render_metal.m, SetDrawState) sets the
// encoder's scissor only when the clip rectangle, in output pixels, intersects the
// render target. A clip rectangle that is empty or wholly off the target leaves the
// PREVIOUS scissor in force, so the draws it was meant to remove land clipped to
// whatever was set before (often the whole view). OpenGL, Direct3D and the software
// renderer clip them all away. First seen at output scale 2: a clip set for a part of
// the view past the target's bottom edge was off the target, and the draws it was to
// remove landed on the screen, on Metal only.
//
// So a clip rectangle that can miss the target is set through set(), and the draws it
// clips are skipped when that returns false (covers() asks without setting anything).
#pragma once

#include <SDL.h>

#include <cmath>

namespace tak::cliprect {

// The output pixels of the current target that the clip rectangle r (logical units,
// relative to the viewport) lets through, as SDL_render.c and the backends compute
// them: rectangle and viewport scaled and floored, the viewport's origin added, the
// result cut to the target. Empty when r covers no pixel.
inline SDL_Rect onTarget(SDL_Renderer* ren, const SDL_Rect& r) {
    float sx = 1, sy = 1;
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_Rect vp{};
    SDL_RenderGetViewport(ren, &vp);
    const auto px = [](int v, float s) { return int(std::floor(double(v) * double(s))); };
    const SDL_Rect out{px(vp.x, sx) + px(r.x, sx), px(vp.y, sy) + px(r.y, sy), px(r.w, sx), px(r.h, sy)};
    int tw = 0, th = 0;
    SDL_GetRendererOutputSize(ren, &tw, &th);   // the target texture's size when one is set
    const SDL_Rect target{0, 0, tw, th};
    SDL_Rect hit{};
    if (!SDL_IntersectRect(&target, &out, &hit)) return SDL_Rect{0, 0, 0, 0};
    return hit;
}

// Does the clip rectangle r let any pixel through?
inline bool covers(SDL_Renderer* ren, const SDL_Rect& r) {
    const SDL_Rect hit = onTarget(ren, r);
    return !SDL_RectEmpty(&hit);
}

// SDL_RenderSetClipRect(ren, &r), false when r lets no pixel through: then the caller
// draws nothing it meant to clip (on Metal they would land under the previous scissor).
// A negative size is set as an empty rectangle (SDL would turn clipping off for it).
inline bool set(SDL_Renderer* ren, const SDL_Rect& r) {
    const SDL_Rect clip{r.x, r.y, r.w < 0 ? 0 : r.w, r.h < 0 ? 0 : r.h};
    SDL_RenderSetClipRect(ren, &clip);
    return covers(ren, clip);
}

}  // namespace tak::cliprect
