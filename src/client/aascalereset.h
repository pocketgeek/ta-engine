#pragma once

// RAII: force the render scale to 1:1 for an off-screen bake, restoring it on exit.
// The whole-frame AA path (main) leaves SDL_RenderSetScale at Sx during draw(); any
// atlas/icon bake must render at 1:1 or its contents come out scaled.
// Extracted from client/main.cpp; kept at global scope so its unqualified use sites
// there are unchanged.

#include <SDL.h>

struct AaScaleReset {
    SDL_Renderer* r; float sx, sy;
    explicit AaScaleReset(SDL_Renderer* rr) : r(rr) {
        SDL_RenderGetScale(r, &sx, &sy);
        SDL_RenderSetScale(r, 1.0f, 1.0f);
    }
    ~AaScaleReset() { SDL_RenderSetScale(r, sx, sy); }
};
