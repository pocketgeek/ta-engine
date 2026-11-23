// hidpi -- window points vs renderer pixels (macOS Retina, Linux native Wayland).
//
// The engine works in renderer OUTPUT pixels throughout: layout, hit tests, the world
// camera. On Windows and on Linux X11 the window is created without
// SDL_WINDOW_ALLOW_HIGHDPI, so window coordinates, mouse positions and output pixels are
// the same numbers (Windows gets real pixels from per-monitor DPI awareness instead). On
// macOS a Retina window opts in with SDL_WINDOW_ALLOW_HIGHDPI (main.cpp), and so does a
// native Wayland window on Linux (client/waylandchoice.h): the window, mouse and
// window-size APIs then speak POINTS while the renderer has backingScale pixels per
// point -- 1 or 2 on macOS, any desktop scale on Wayland (1.25, 1.5, 2, ...). The main event loops already map mouse events with SDL_RenderWindowToLogical;
// these helpers cover the places that read the mouse or size the window directly.
// Every one is an identity when the scale is 1.
#pragma once

#include "client/settings.h"

#include <SDL.h>

namespace tak::hidpi {

// Output pixels per window point for this renderer's window.
inline float scale(SDL_Renderer* ren) {
    SDL_Window* win = ren ? SDL_RenderGetWindow(ren) : nullptr;
    if (!win) return 1.0f;
    int ww = 0, wh = 0, ow = 0, oh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    if (SDL_GetRendererOutputSize(ren, &ow, &oh) != 0) return 1.0f;
    return backingScale(ww, ow);
}
inline float scale(SDL_Window* win) { return scale(win ? SDL_GetRenderer(win) : nullptr); }

// A window-point coordinate (mouse event, SDL_GetMouseState) in output pixels.
inline int toPixels(SDL_Renderer* ren, int points) {
    const float s = scale(ren);
    return s == 1.0f ? points : int(float(points) * s);
}
// SDL_GetMouseState, in output pixels.
inline void mousePixels(SDL_Renderer* ren, int& x, int& y) {
    SDL_GetMouseState(&x, &y);
    const float s = scale(ren);
    if (s != 1.0f) { x = int(float(x) * s); y = int(float(y) * s); }
}
// An output-pixel rectangle in window points (SDL_SetTextInputRect).
inline SDL_Rect toPoints(SDL_Renderer* ren, SDL_Rect r) {
    const float s = scale(ren);
    if (s == 1.0f) return r;
    return SDL_Rect{int(float(r.x) / s), int(float(r.y) / s),
                    pixelsToPoints(r.w, s), pixelsToPoints(r.h, s)};
}

}  // namespace tak::hidpi
