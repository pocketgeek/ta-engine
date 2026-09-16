#pragma once
// Central GPU-texture memory accountant for the SDL2 client. Render-thread only (every
// SDL texture in src/client is created/destroyed on the render thread), so no locking.
// NONE of this is folded into World::stateHash() -- it is pure display state, so the
// deterministic lockstep sim is untouched (the headless referee never builds a GameView).
//
// Purpose: bound taclient's texture VRAM so a big scene (e.g. a 4K 40k-unit battle) can
// never exhaust the GPU and trigger the NVKMS "Failed to allocate" storm that starves the
// Wayland compositor (whole-screen flicker) and stalls present (frozen screen).
//
// DYNAMIC: the cap self-calibrates. It starts at a generous default; the instant a real
// SDL_CreateTexture FAILS (the only portable "out of VRAM" signal there is), the cap is
// tightened to just under what we currently hold -- so on a VRAM-tight machine it discovers
// that machine's real ceiling and evicts down to it, while a roomy GPU never trips it and
// keeps the generous default. No NVIDIA/NVML dependency.
#include <SDL.h>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace gpuvram {

inline size_t   g_bytes = 0;                     // running total of live texture bytes
inline size_t   g_cap   = size_t(1280) << 20;    // budget; default 1.25 GiB, tightens on failure
inline size_t   g_floor = size_t(384) << 20;     // never auto-tighten below this (essentials)
inline std::unordered_map<SDL_Texture*, size_t> g_size;   // per-texture byte size
inline uint32_t g_backoffUntil = 0;              // SDL_GetTicks() until which bakes pause

inline size_t bytes() { return g_bytes; }
inline size_t cap()   { return g_cap; }
inline size_t count() { return g_size.size(); }   // tracked textures (teardown-leak check)
inline void   setCap(size_t b) { g_cap = b; if (g_floor > b) g_floor = b; }
inline bool   wouldFit(size_t add) { return g_bytes + add <= g_cap; }

// Are we in a post-failure backoff? (bakes should skip while true)
inline bool blocked() { return g_backoffUntil && SDL_GetTicks() < g_backoffUntil; }

// A texture allocation failed = out of VRAM right now. Clamp the cap to just under what we
// already hold (self-calibrating), and pause bakes for 3s so we don't hammer the driver.
inline void noteFail() {
    if (g_bytes > g_floor) {
        size_t adapted = g_bytes - (g_bytes >> 3);   // 87.5% of current usage
        if (adapted < g_floor) adapted = g_floor;
        if (adapted < g_cap) {
            g_cap = adapted;
            std::fprintf(stderr, "gpu: VRAM alloc failed -- tightening texture cap to %zu MiB\n",
                         g_cap >> 20);
        }
    }
    g_backoffUntil = SDL_GetTicks() + 3000;
}

// Tracked create/destroy. Every SDL_CreateTexture/SDL_DestroyTexture in the client routes
// through these so g_bytes stays exact (all client textures are 4 bytes/px: RGBA32/RGBA8888).
inline SDL_Texture* create(SDL_Renderer* r, Uint32 fmt, int access, int w, int h) {
    SDL_Texture* t = SDL_CreateTexture(r, fmt, access, w, h);
    if (t) { size_t b = size_t(w) * size_t(h) * 4; g_bytes += b; g_size[t] = b; }
    return t;
}
inline void destroy(SDL_Texture* t) {
    if (!t) return;
    if (auto it = g_size.find(t); it != g_size.end()) { g_bytes -= it->second; g_size.erase(it); }
    SDL_DestroyTexture(t);
}

}  // namespace gpuvram
