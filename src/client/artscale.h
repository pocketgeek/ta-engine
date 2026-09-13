// artscale -- edge-directed 2x upscale for the retail sprite art.
//
// The shipped art is 1999-era and authored for a 640x480 screen: cursors are 20-30px,
// GUI panels and faction backdrops not much better. On a 4K panel, magnified by the UI
// scale, every one of those pixels is a visible block.
//
// STATIC ART ONLY -- GUI, panels, buttons, faction/menu backdrops, weapon pics,
// cursors. Deliberately NOT the unit and feature frames: those go through the shared
// atlas and there are hundreds of them, so 4x the pixels would be real VRAM on a card
// this engine has already been caught starving, for art that is usually on screen at a
// fraction of its authored size anyway. SDL's linear filter (the
// existing `bilinear` option) smooths them at DRAW time, but it is interpolating 1x
// data, so it can only blur -- a hard diagonal becomes a soft diagonal of the same
// staircase. An edge-directed scaler does better because it INFERS the edge from the
// neighbourhood: where two neighbours agree with each other and disagree with the
// centre, that is a corner, and the output subpixel there gets blended instead of
// copied. Diagonals come out smooth rather than blurred.
//
// This runs ONCE per texture, at load. Nothing here is on a frame path.
//
// The rule is the classic Eagle/hq2x corner test, kept deliberately small:
//
//       A B C          each output 2x2 subpixel looks at the two neighbours
//       D E F          adjacent to its corner -- top-left looks at D and B --
//       G H I          and blends when they match each other but not E.
//
// Working in premultiplied alpha matters: the art is full of fully transparent
// pixels whose RGB is arbitrary (often black or the palette's key colour), and
// blending those straight would drag dark fringes into every silhouette edge.

#pragma once

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "client/gpuvram.h"

namespace tak::art {

// Are two RGBA pixels close enough to count as "the same colour" for edge detection?
// Alpha is weighted hardest: a transparent/opaque boundary IS the silhouette edge, and
// it must never be treated as a flat region.
inline bool similar(const uint8_t* a, const uint8_t* b) {
    const int da = std::abs(int(a[3]) - int(b[3]));
    if (da > 24) return false;
    // Both effectively transparent -> same, whatever the junk RGB underneath says.
    if (a[3] < 8 && b[3] < 8) return true;
    const int dr = std::abs(int(a[0]) - int(b[0]));
    const int dg = std::abs(int(a[1]) - int(b[1]));
    const int db = std::abs(int(a[2]) - int(b[2]));
    return (dr * 2 + dg * 3 + db) < 120;   // luma-ish weighting
}

// Blend three pixels 2:1:1 (centre dominant) in PREMULTIPLIED space, then unpremultiply.
// Straight-alpha averaging would pull the RGB of transparent neighbours into the edge.
inline void blend3(const uint8_t* e, const uint8_t* p, const uint8_t* q, uint8_t* out) {
    const int ae = e[3], ap = p[3], aq = q[3];
    const int a = (ae * 2 + ap + aq) / 4;
    if (a == 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    for (int k = 0; k < 3; ++k) {
        const int pm = int(e[k]) * ae * 2 + int(p[k]) * ap + int(q[k]) * aq;
        out[k] = uint8_t(std::min(255, pm / (4 * a)));
    }
    out[3] = uint8_t(a);
}

// Upscale `src` (w*h, RGBA32) to 2w*2h into `dst`. dst is resized.
inline void upscale2x(const std::vector<uint8_t>& src, int w, int h,
                      std::vector<uint8_t>& dst) {
    const int dw = w * 2, dh = h * 2;
    dst.assign(size_t(dw) * size_t(dh) * 4, 0);
    auto at = [&](int x, int y) -> const uint8_t* {
        if (x < 0) x = 0; else if (x >= w) x = w - 1;      // clamp: edges repeat rather
        if (y < 0) y = 0; else if (y >= h) y = h - 1;      // than sample off-image
        return &src[(size_t(y) * size_t(w) + size_t(x)) * 4];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* E = at(x, y);
            const uint8_t* B = at(x, y - 1);
            const uint8_t* D = at(x - 1, y);
            const uint8_t* F = at(x + 1, y);
            const uint8_t* H = at(x, y + 1);
            uint8_t o[4][4];
            for (int k = 0; k < 4; ++k) {
                o[0][k] = E[k]; o[1][k] = E[k]; o[2][k] = E[k]; o[3][k] = E[k];
            }
            // A corner is "cut" when its two adjacent neighbours agree with each other
            // and differ from the centre. B==H or D==F means we are inside a run, not on
            // a corner, so leave it alone -- that guard is what keeps straight lines
            // straight instead of eroding them.
            if (!similar(B, H) && !similar(D, F)) {
                if (similar(D, B) && !similar(E, D)) blend3(E, D, B, o[0]);
                if (similar(B, F) && !similar(E, B)) blend3(E, B, F, o[1]);
                if (similar(D, H) && !similar(E, D)) blend3(E, D, H, o[2]);
                if (similar(H, F) && !similar(E, H)) blend3(E, H, F, o[3]);
            }
            uint8_t* row0 = &dst[(size_t(y * 2) * size_t(dw) + size_t(x * 2)) * 4];
            uint8_t* row1 = &dst[(size_t(y * 2 + 1) * size_t(dw) + size_t(x * 2)) * 4];
            for (int k = 0; k < 4; ++k) {
                row0[k] = o[0][k]; row0[4 + k] = o[1][k];
                row1[k] = o[2][k]; row1[4 + k] = o[3][k];
            }
        }
    }
}

// ---- the one switch, and the texture helper ----------------------------------------
//
// Sampled ONCE at startup from Settings::smoothArt and then left alone. It must not be
// re-read per texture: art loaded before a mid-session toggle keeps the factor it was
// built with, and a caller that laid out from the OLD factor while asking the NEW one
// would misplace it. Hence the Options row says the change needs a restart.
inline bool g_smoothArt = false;
inline void setSmoothArt(bool on) { g_smoothArt = on; }

// The factor the CURSORS are built at, sampled from Settings::cursorScale at startup.
// Cursors are magnified up to 8x by that setting, and a source that still has to be
// magnified stays soft -- overshooting so the draw DOWNSAMPLES is what makes the edge
// crisp as well as smooth. Rounded up to a power of two and clamped to [2,8]; a 29x32
// cursor at 8x is ~240 KB, which is affordable for the dozen or so of them and would
// not be on the unit atlas. Sampled once, like the switch above, so changing cursor
// size mid-session rebuilds nothing until a restart.
inline int g_cursorFactor = 2;
inline void setCursorFactor(int cursorScale) {
    int f = 2;
    while (f < cursorScale && f < 8) f *= 2;
    g_cursorFactor = f;
}

// Upload `rgba` as a texture, edge-directed-upscaled 2x when smoothing is on.
//
// `appliedFactor` reports what actually happened, and callers that derive LAYOUT from
// the texture's dimensions must divide by it -- MainMenu draws its buttons at native
// texture size, so without this the art would silently render twice as large. It is an
// out-param rather than a global read because the 2x allocation can fail under VRAM
// pressure and fall back to 1x, and only the call itself knows which it got.
// `want` is the upscale factor to aim for: 2, 4 or 8, reached by repeated 2x passes.
// 2 suits GUI art, which is drawn at roughly its authored size. Cursors ask for more,
// because cursorScale magnifies them up to 8x and a source that still has to be
// MAGNIFIED is soft -- overshooting so the draw DOWNSAMPLES is what makes an edge
// crisp as well as smooth. They are a few hundred pixels each, so it is affordable
// there and would not be on the unit atlas.
inline SDL_Texture* makeTexture(SDL_Renderer* ren, const std::vector<uint8_t>& rgba,
                                int w, int h, int* appliedFactor = nullptr,
                                int want = 2) {
    if (appliedFactor) *appliedFactor = 1;
    if (w <= 0 || h <= 0) return nullptr;
    // Cap the source size: past this the art is already big enough that the blockiness
    // this exists to fix is not visible, and 4x the VRAM is not worth it.
    if (g_smoothArt && w <= 1024 && h <= 1024 &&
        rgba.size() >= size_t(w) * size_t(h) * 4) {
        std::vector<uint8_t> up;
        upscale2x(rgba, w, h, up);
        int fw = w * 2, fh = h * 2;
        for (int f = 2; f < want && fw <= 2048 && fh <= 2048; f *= 2) {
            std::vector<uint8_t> nxt;
            upscale2x(up, fw, fh, nxt);
            up.swap(nxt); fw *= 2; fh *= 2;
        }
        if (SDL_Texture* t = gpuvram::create(ren, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, fw, fh)) {
            SDL_UpdateTexture(t, nullptr, up.data(), fw * 4);
            // Linear, so the 2x data resolves smoothly at whatever size it is drawn.
            SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            if (appliedFactor) *appliedFactor = fw / w;
            return t;
        }
        // VRAM said no: fall through and build it at 1x rather than losing the art.
    }
    SDL_Texture* t = gpuvram::create(ren, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

}  // namespace tak::art
