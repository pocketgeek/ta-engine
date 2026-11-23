#pragma once
// Central GPU memory accountant and safety-net cap for the SDL2 client. Render-thread
// only (every SDL texture in src/client is created/destroyed on the render thread), so
// no locking. NONE of this is folded into World::stateHash() -- it is pure display
// state, and the headless referee never builds a GameView.
//
// WHAT IS COUNTED. g_bytes is the running total the stats panel and benchmark show:
//   * every texture made through create()/destroy() (w*h*4 -- all client textures are
//     4 bytes/px: RGBA32/ABGR8888/ARGB8888);
//   * GL buffers added with addBuffer()/removeBuffer(): the retained fog mesh
//     (GeometrySubmit) and the stream-capture PBOs + scaling renderbuffer (streaming.cpp).
// NOT counted: the swapchain/backbuffers (~63 MiB each at 7680x2160), SDL's own
// internal objects, and driver padding and retention -- nvidia-smi measured real use at
// 1.2-2.2x this total while terrain chunks churn. The Cartographer is a separate
// executable and does not use this.
//
// WHY A CAP. It was added (2026-09) to stop unbounded 64 MiB sprite/impostor pages
// exhausting an 8 GB card behind a 7680x2160 desktop (the NVKMS allocation storm that
// starves the compositor: whole-screen flicker, frozen present). Those pages and the
// window-sized AA targets are gone. What bounds VRAM now is each subsystem's own ceiling:
//   terrain chunks 40% of the limit, 256 MiB-2 GiB  map sections: those the view needs while
//                                                    chunks bake, 64 MiB idle (mapview.cpp)
//   palette pages 16 x 16 MiB                         shadow silhouettes 4 x 16 MiB (+12
//                                                    extended pages, only within 3/4 of
//                                                    the cap: shadowatlas.h)
//   distant models 4 x 16 MiB                         shadow coverage 16 MiB
//   GeometrySubmit 64 MiB per retained buffer         colour atlases ~10.7 MiB per slot
// which add up to ~1 GiB in a pathological game; typical games account 0.4-0.7 GiB.
// The cap is the backstop above all of them (and for any future allocator) and the
// player's Options GPU MEMORY LIMIT (setLimit; 1.25 GiB by default, UNLIMITED = no cap).
//
// WHO OBEYS IT. Growable caches check wouldFit() (and blocked()) before allocating and
// fall back when told no: terrain chunks and sections evict their own LRU first (and
// shed proactively when the cap drops below what is held), palette/shadow/distant pages
// stop growing and draw through their fallbacks. Optimizations that only replace a
// slower path -- the body vertex arena, extended shadow pages -- stay below 3/4 of the
// cap and give their memory back under pressure (above 7/8, or a tightened cap).
// ESSENTIALS go past it: a colour atlas (no atlas = that colour's units are not
// drawn) first asks the terrain caches to shed, then allocates anyway. Content-bounded
// art (model/feature/effect frames, GUI, fonts, icons, movies) is created unchecked. So
// the total can exceed a low cap a little.
//
// noteFail(): a create returned NULL. Pause every bake for 3 s, clamp the cap to 87.5%
// of what is held (not below 384 MiB), and lift that tightening after 60 s without
// another failure (doubling per re-tightening, up to 8 min) -- a transient shortage must
// not cost quality for the rest of the session. Where it can fire depends on the
// renderer: D3D11/12, Metal and the software renderer return NULL when an allocation
// fails. SDL's OpenGL and GLES2 renderers do NOT: GL_CheckAllErrors (SDL 2.32,
// render/opengl/SDL_render_gl.c, likewise opengles2) returns 0 unless the context has
// SDL_GL_CONTEXT_DEBUG_FLAG, which takclient never requests, so a GL_OUT_OF_MEMORY from
// glTexImage2D never fails SDL_CreateTexture, and FBO completeness is only checked when
// a target is bound. (NVIDIA's Linux driver also defers allocation and spills to system
// memory.) On GL the only caller that can fire is GeometrySubmit's GL_BUFFER_SIZE check;
// there the subsystem ceilings and the cap are the protection, not failure detection.
#include <SDL.h>
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include "client/gpuvrampolicy.h"

namespace gpuvram {

inline size_t g_bytes = 0;                                  // live texture + counted buffer bytes
inline Policy g_policy;                                     // limit / cap / backoff (gpuvrampolicy.h)
inline std::unordered_map<SDL_Texture*, size_t> g_size;     // per-texture byte size
inline uint64_t g_creates = 0, g_destroys = 0;              // cumulative (render telemetry)

inline uint64_t nowMs() { return SDL_GetTicks64(); }
inline void relax() {
    if (!g_policy.relax(nowMs())) return;
    if (g_policy.cap == kUnlimited)
        std::fprintf(stderr, "gpu: no VRAM failure for %llus -- texture cap lifted (unlimited)\n",
                     (unsigned long long)(g_policy.relaxWait / 1000));
    else
        std::fprintf(stderr, "gpu: no VRAM failure for %llus -- texture cap back to %zu MiB\n",
                     (unsigned long long)(g_policy.relaxWait / 1000), g_policy.cap >> 20);
}

inline size_t bytes() { return g_bytes; }
inline size_t cap()   { relax(); return g_policy.cap; }      // kUnlimited when there is none
inline size_t limit() { return g_policy.limit; }
inline bool   tightened() { relax(); return g_policy.tightened(); }
inline size_t count() { return g_size.size(); }   // tracked textures (teardown-leak check)
inline bool   wouldFit(size_t add) { relax(); return g_policy.fits(g_bytes, add); }
inline bool   overCap() { return !wouldFit(0); }

// The player's GPU MEMORY LIMIT (Options). Live: raising it lets the caches grow on
// their next allocation; lowering it makes them shed what they can next frame.
inline void setLimit(size_t b) {
    if (!g_policy.setLimit(b)) return;
    if (g_policy.limit == kUnlimited) std::fprintf(stderr, "gpu: texture memory limit unlimited\n");
    else std::fprintf(stderr, "gpu: texture memory limit %zu MiB\n", g_policy.limit >> 20);
}

// Are we in a post-failure backoff? (bakes should skip while true)
inline bool blocked() { return g_policy.blocked(nowMs()); }

// A texture/buffer allocation failed. See the header comment.
inline void noteFail() {
    if (g_policy.fail(g_bytes, nowMs()))
        std::fprintf(stderr, "gpu: VRAM alloc failed -- tightening texture cap to %zu MiB for now\n",
                     g_policy.cap >> 20);
}

// Tracked create/destroy. Every texture the client keeps should route through these so
// the total stays honest; a raw SDL_CreateTexture is invisible to it.
inline SDL_Texture* create(SDL_Renderer* r, Uint32 fmt, int access, int w, int h) {
    SDL_Texture* t = SDL_CreateTexture(r, fmt, access, w, h);
    if (t) { size_t b = size_t(w) * size_t(h) * 4; g_bytes += b; g_size[t] = b; ++g_creates; }
    return t;
}
inline void destroy(SDL_Texture* t) {
    if (!t) return;
    if (auto it = g_size.find(t); it != g_size.end()) { g_bytes -= it->second; g_size.erase(it); ++g_destroys; }
    SDL_DestroyTexture(t);
}
// Non-texture GPU memory the client allocates itself (GL buffers, renderbuffers).
inline void addBuffer(size_t b) { g_bytes += b; }
inline void removeBuffer(size_t b) { g_bytes -= std::min(b, g_bytes); }

}  // namespace gpuvram
