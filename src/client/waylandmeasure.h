// waylandmeasure -- Linux: the numbers tak::wayland::advanceDisplay compares, read from SDL.
// See client/waylandchoice.h for what they decide.
#pragma once

#include "client/settings.h"
#include "client/waylandchoice.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace tak::wayland {

// Width of the desktop as SDL's driver reports it: the union of the display bounds. On
// the x11 driver that is XWayland's screen (real pixels or not); on the wayland driver it
// is the logical (xdg-output) desktop, in points.
inline int desktopWidth() {
    const int n = SDL_GetNumVideoDisplays();
    int lo = 0, hi = 0;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        SDL_Rect r{};
        if (SDL_GetDisplayBounds(i, &r) != 0 || r.w <= 0) continue;
        lo = any ? std::min(lo, r.x) : r.x;
        hi = any ? std::max(hi, r.x + r.w) : r.x + r.w;
        any = true;
    }
    return any ? hi - lo : 0;
}

// The scale the window's display has: its native mode over its logical desktop size (SDL
// lists the native mode first when the desktop is scaled). 1 when not scaled.
inline float displayScaleOf(SDL_Window* win) {
    const int d = std::max(0, SDL_GetWindowDisplayIndex(win));
    SDL_DisplayMode desk{}, top{};
    if (SDL_GetDesktopDisplayMode(d, &desk) != 0 || desk.w <= 0) return 1.0f;
    if (SDL_GetDisplayMode(d, 0, &top) != 0 || top.w <= desk.w) return 1.0f;
    return float(top.w) / float(desk.w);
}

// A native window's scale, SETTLED: the compositor reports it only once the surface is on
// an output, after the window is shown and has a frame, so a read at creation can still
// say 1. Present (black) frames and take in events until the window's scale matches its
// display's, up to `budgetMs`; on time-out a scale above 1 is the compositor's (accepted),
// while 1 against a scaled display is not settled: 0, and nothing is recorded this start.
inline float settledNativeScale(SDL_Window* win, SDL_Renderer* ren, int budgetMs = 1500) {
    const Uint32 t0 = SDL_GetTicks();
    float got = 1.0f;
    for (;;) {
        SDL_PumpEvents();
        // Asked again each time: until the surface is on an output SDL may name another
        // display than the one the compositor puts the window on.
        const float want = displayScaleOf(win);
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        if (SDL_GetRendererOutputSize(ren, &ow, &oh) == 0) got = backingScale(ww, ow);
        if (std::abs(got - want) < 0.01f) return got;
        if (SDL_GetTicks() - t0 >= Uint32(budgetMs)) break;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        SDL_Delay(10);
    }
    return got > 1.005f ? got : 0.0f;
}

}  // namespace tak::wayland
