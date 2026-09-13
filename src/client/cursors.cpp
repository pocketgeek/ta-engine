#include "client/cursors.h"

#include "client/artscale.h"
#include "client/settings.h"
#include "client/gpuvram.h"

#include "gaf/gaf.h"
#include "hpi/hpi.h"

#include <cctype>
#include <string>
#include <unordered_map>

namespace tak {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// CursorId -> cursors.gaf sequence name (matched case-insensitively, since the GAF
// mixes cases: "CursorMove" vs "cursorselect"). Derived from KINGDOMS.icd's loader.
const char* seqName(CursorId c) {
    switch (c) {
        case CursorId::Normal:    return "cursornormal";
        case CursorId::Select:    return "cursorselect";
        case CursorId::Move:      return "CursorMove";
        case CursorId::Attack:    return "CursorAttack";
        case CursorId::Airstrike: return "cursorairstrike";
        case CursorId::TooFar:    return "cursortoofar";
        case CursorId::Patrol:    return "CursorPatrol";
        case CursorId::Defend:    return "CursorDefend";
        case CursorId::Repair:    return "cursorrepair";
        case CursorId::Load:      return "Cursorload";
        case CursorId::Unload:    return "CursorUnload";
        case CursorId::Reclaim:   return "Cursorreclamate";
        case CursorId::Revive:    return "cursorrevive";
        case CursorId::FindSite:  return "cursorfindsite";
        case CursorId::Green:     return "cursorgrn";
        case CursorId::Red:       return "cursorred";
        case CursorId::Hourglass: return "cursorhourglass";
        case CursorId::PathIcon:  return "pathicon";
        default:                  return "";
    }
}

}  // namespace

CursorSet::~CursorSet() {
    releaseHardware();
    for (auto& frames : anims_)
        for (auto& f : frames)
            if (f.tex) gpuvram::destroy(f.tex);
}

bool CursorSet::load(SDL_Renderer* ren, const hpi::Vfs& vfs, const Settings* settings) {
    if (!ren) return false;
    std::vector<uint8_t> gafBytes, palBytes;
    try {
        gafBytes = vfs.read("anims/cursors.gaf");
        palBytes = vfs.read("palettes/cursors.pcx");
    } catch (...) { return false; }

    gaf::Palette pal;
    std::vector<gaf::Sequence> seqs;
    try {
        pal = gaf::Palette::fromBytes(palBytes, "palettes/cursors.pcx");
        // -1: honour each frame's own transparency index (the cursor art is authored
        // that way, so backgrounds decode straight to alpha=0 -- verified on export).
        seqs = gaf::load(gafBytes, pal, -1, "anims/cursors.gaf");
    } catch (...) { return false; }

    std::unordered_map<std::string, const gaf::Sequence*> byName;
    for (auto& s : seqs) byName[lower(s.name)] = &s;

    for (size_t i = 0; i < size_t(CursorId::Count); ++i) {
        auto it = byName.find(lower(seqName(CursorId(i))));
        if (it == byName.end()) continue;               // leave empty; draw() falls back
        for (const auto& fr : it->second->frames) {
            if (fr.width <= 0 || fr.height <= 0) continue;
            // Smoothing OFF keeps nearest, i.e. the crisp 1:1 retail pixel cursor. ON
            // builds it at 2x and lets it resolve linearly, which matters here more than
            // anywhere: the cursor is ~24px of 1999 art drawn at cursorScale on a 4K
            // panel, so its stair-steps are the most visible in the game.
            int fac = 1;
            SDL_Texture* t = tak::art::makeTexture(ren, fr.rgba, fr.width, fr.height, &fac,
                                                   tak::art::g_cursorFactor);
            if (!t) continue;
            if (fac == 1) SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
            // LOGICAL size stays the 1x frame: hotspot, offsets and the drawn size are
            // all authored in those units, and the texture being 2x is invisible to them.
            anims_[i].push_back({t, fr.width, fr.height, fr.xoff, fr.yoff, fr.rgba});
        }
    }
    // Reconstruct now, here, off the frame path -- but only for the HARDWARE cursor.
    // The software path draws the textures above, which makeTexture has already
    // upscaled, so this work would go unread there.
    if (settings && settings->hardwareCursor) precompute(settings->cursorScale);
    // Need at least the normal pointer to justify taking over from the OS cursor.
    ok_ = !anims_[size_t(CursorId::Normal)].empty();
    return ok_;
}

void CursorSet::drawFrame(SDL_Renderer* ren, CursorId c, size_t frame, int x, int y,
                          int scale, SDL_Color tint) const {
    if (!ok_ || !ren || size_t(c) >= anims_.size()) return;
    const auto& frames = anims_[size_t(c)];
    if (frames.empty()) return;
    if (scale < 1) scale = 1;
    const Frame& f = frames[frame % frames.size()];
    SDL_SetTextureColorMod(f.tex, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(f.tex, tint.a);
    SDL_Rect dst{ x - f.hx * scale, y - f.hy * scale, f.w * scale, f.h * scale };
    SDL_RenderCopy(ren, f.tex, nullptr, &dst);
}

void CursorSet::draw(SDL_Renderer* ren, CursorId c, int mouseX, int mouseY, int scale, SDL_Color tint) {
    if (!ok_ || !ren) return;
    if (size_t(c) >= anims_.size() || anims_[size_t(c)].empty())
        c = CursorId::Normal;                           // fall back if this cursor didn't load
    const auto& frames = anims_[size_t(c)];
    if (frames.empty()) return;
    if (scale < 1) scale = 1;

    const uint64_t now = SDL_GetTicks64();
    if (c != cur_) { cur_ = c; animStartMs_ = now; }    // restart animation on a change
    size_t idx = 0;
    if (frames.size() > 1)
        idx = size_t((now - animStartMs_) * kFps / 1000) % frames.size();

    const Frame& f = frames[idx];
    // Set the mod every draw (default white = no-op) so a previous tint never lingers.
    SDL_SetTextureColorMod(f.tex, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(f.tex, tint.a);
    // Magnify sprite AND hotspot by `scale` so the anchor stays on the pointer.
    SDL_Rect dst{ mouseX - f.hx * scale, mouseY - f.hy * scale, f.w * scale, f.h * scale };
    SDL_RenderCopy(ren, f.tex, nullptr, &dst);
}

namespace {

// Bake one frame's RGBA into a scaled, tinted SDL_Cursor. Hotspot is scaled to match.
// Returns nullptr on failure.
//
// This is the HARDWARE cursor path (SDL_CreateColorCursor), and it does not go anywhere
// near the textures the smooth-art option upscales -- it builds an OS cursor straight
// from the 1x frame. It used to nearest-replicate every pixel `scale` times, so with
// hardwareCursor on, smooth art changed the cursor not at all however it was set.
//
// Now, when smoothing is on: reconstruct the edges to a power-of-two factor that
// OVERSHOOTS the drawn size, then area-average down onto the exact target. The
// downsample is what antialiases. Smoothing off keeps the old nearest replication,
// which is the crisp retail pixel cursor.
SDL_Cursor* bakeCursor(const std::vector<uint8_t>& rgba, const std::vector<uint8_t>* smoothed,
                       int w, int h, int hx, int hy, int scale, SDL_Color tint) {
    if (rgba.size() < size_t(w) * size_t(h) * 4 || w <= 0 || h <= 0) return nullptr;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w * scale, h * scale, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    // `smoothed` is the prepared reconstruction (see CursorSet::precompute) or empty.
    // Nothing expensive happens here: this runs on FIRST USE of a (cursor,tint) pair,
    // which is mid-frame when you hover a new action, so it must stay a tint multiply.
    const bool haveSmooth = smoothed && smoothed->size() >= size_t(w) * size_t(h) *
                                        size_t(scale) * size_t(scale) * 4;
    const uint8_t* base = haveSmooth ? smoothed->data() : rgba.data();
    const int bw = haveSmooth ? w * scale : w;
    for (int y = 0; y < h * scale; ++y) {
        auto* dst = static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch;
        for (int x = 0; x < w * scale; ++x) {
            // Either 1:1 into the already-resampled buffer, or nearest into the 1x frame.
            const uint8_t* p = (bw == w * scale)
                ? base + (size_t(y) * size_t(bw) + size_t(x)) * 4
                : base + (size_t(y / scale) * size_t(w) + size_t(x / scale)) * 4;
            dst[0] = uint8_t(p[0] * tint.r / 255);
            dst[1] = uint8_t(p[1] * tint.g / 255);
            dst[2] = uint8_t(p[2] * tint.b / 255);
            dst[3] = uint8_t(p[3] * tint.a / 255);
            dst += 4;
        }
    }
    SDL_Cursor* cur = SDL_CreateColorCursor(s, hx * scale, hy * scale);
    SDL_FreeSurface(s);
    return cur;
}

}  // namespace

// Reconstruct every frame at the size it will be DRAWN, once. This is the expensive
// half of the smooth cursor -- repeated 2x passes plus an area-average resample -- and
// it is tint-independent, so doing it here means the per-(cursor,tint) bake that runs
// mid-frame stays a tint multiply.
//
// It was NOT here originally: the bake was entirely lazy, so the reconstruction landed
// inside a rendered frame the first time you hovered a new action. Measured at CURSOR
// SIZE 8 that was 17.3 ms for the 20-frame patrol cursor -- more than a whole frame's
// budget, as a hitch on hover. Up front it is 104.9 ms for all 141 frames, once, inside
// a load that already takes seconds.
void CursorSet::precompute(int scale) {
    if (scale < 1) scale = 1;
    if (!tak::art::g_smoothArt || scale <= 1) return;   // nothing to reconstruct
    for (auto& anim : anims_)
        for (auto& f : anim) {
            if (f.smoothScale == scale || f.w <= 0 || f.h <= 0) continue;
            std::vector<uint8_t> up;
            tak::art::upscale2x(f.rgba, f.w, f.h, up);
            int fw = f.w * 2, fh = f.h * 2;
            // Overshoot to at least TWICE the drawn size, not merely up to it. At a
            // power-of-two CURSOR SIZE, stopping at the drawn size makes the resample
            // 1:1 -- reconstructed edges but no area-averaging, which at size 8 still
            // reads as "not antialiased". One more doubling guarantees several source
            // pixels per destination pixel, which is where the smoothing comes from.
            while (fw / f.w < scale * 2 && fw < 4096) {
                std::vector<uint8_t> nxt;
                tak::art::upscale2x(up, fw, fh, nxt);
                up.swap(nxt); fw *= 2; fh *= 2;
            }
            tak::art::resample(up, fw, fh, f.smooth, f.w * scale, f.h * scale);
            f.smoothScale = scale;
        }
}

bool CursorSet::applyHardware(CursorId c, int scale, SDL_Color tint) {
    if (!ok_) return false;
    if (size_t(c) >= anims_.size() || anims_[size_t(c)].empty()) c = CursorId::Normal;
    const auto& frames = anims_[size_t(c)];
    if (frames.empty()) return false;
    if (scale < 1) scale = 1;

    if (scale != hwScale_) {                    // rebuild on scale change
        releaseHardware();
        hwScale_ = scale;
        precompute(scale);                      // one stall, not one per hovered action
    }

    const uint32_t packed = (uint32_t(tint.r) << 24) | (uint32_t(tint.g) << 16) |
                            (uint32_t(tint.b) << 8) | uint32_t(tint.a);
    const uint64_t key = (uint64_t(c) << 32) | packed;
    auto it = hw_.find(key);
    if (it == hw_.end()) {                       // lazily bake this (cursor,tint) set
        std::vector<SDL_Cursor*> built;
        built.reserve(frames.size());
        for (const auto& f : frames) {
            SDL_Cursor* cur = bakeCursor(f.rgba, f.smoothScale == scale ? &f.smooth : nullptr,
                                         f.w, f.h, f.hx, f.hy, scale, tint);
            if (!cur) {                          // platform rejected it -> unwind, fall back
                for (SDL_Cursor* b : built) SDL_FreeCursor(b);
                return false;
            }
            built.push_back(cur);
        }
        it = hw_.emplace(key, std::move(built)).first;
    }
    const auto& curs = it->second;
    if (curs.empty()) return false;

    const uint64_t now = SDL_GetTicks64();
    if (c != hwCur_) { hwCur_ = c; hwStartMs_ = now; }   // restart animation on a change
    size_t idx = curs.size() > 1 ? size_t((now - hwStartMs_) * kFps / 1000) % curs.size() : 0;

    if (curs[idx] != hwSet_) { hwSet_ = curs[idx]; SDL_SetCursor(hwSet_); }
    return true;
}

void CursorSet::releaseHardware() {
    for (auto& kv : hw_)
        for (SDL_Cursor* c : kv.second)
            if (c) SDL_FreeCursor(c);
    hw_.clear();
    hwScale_ = 0;
    hwSet_ = nullptr;
    hwCur_ = CursorId::Count;
    SDL_SetCursor(SDL_GetDefaultCursor());   // don't leave a freed cursor active
}

}  // namespace tak
