// Animated colour-atlas maintenance (render optimization workstream B): the
// incremental per-atlas, per-region repaint must leave every atlas byte-identical
// -- content, gutters and padding -- to a complete repaint, and to the previous
// "repaint every animated region of every atlas" scheme once that scheme has run.
// Covers frame changes at different cadences, non-looping expiry, late atlas
// creation, renderer resets, asset-generation changes, multiple player-colour
// atlases, and the scale-mode protection that replaced the per-region flush.
#include "client/animatedatlas.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; }
}

constexpr int kW = 64, kH = 48;
SDL_Renderer* ren = nullptr;

using Pixels = std::vector<uint8_t>;

SDL_Texture* makeTexture(int w, int h, uint32_t seed, SDL_ScaleMode scale = SDL_ScaleModeNearest) {
    Pixels p(size_t(w) * h * 4);
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < p.size(); i += 4) {
        x = x * 1664525u + 1013904223u;
        p[i] = uint8_t(x >> 24); p[i + 1] = uint8_t(x >> 16); p[i + 2] = uint8_t(x >> 8);
        // Opaque, transparent with non-black RGB, and partial alpha: a copy with the
        // wrong blend mode changes at least one of these.
        const uint8_t a[4] = {255, 0, 128, 255};
        p[i + 3] = a[(x >> 3) & 3];
    }
    SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(t, nullptr, p.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);   // as GameView::loadTextures
    SDL_SetTextureScaleMode(t, scale);
    return t;
}

Pixels textureBytes(SDL_Texture* t) {
    int w = 0, h = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &w, &h);
    SDL_Texture* target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
    SDL_Texture* prev = SDL_GetRenderTarget(ren);
    SDL_SetRenderTarget(ren, target);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
    SDL_BlendMode b; SDL_GetTextureBlendMode(t, &b);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(ren, t, nullptr, nullptr);
    SDL_SetTextureBlendMode(t, b);
    Pixels p(size_t(w) * h * 4);
    SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, p.data(), w * 4);
    SDL_SetRenderTarget(ren, prev);
    SDL_DestroyTexture(target);
    return p;
}

Pixels readAtlas(SDL_Texture* atlas) {
    Pixels p(size_t(kW) * kH * 4);
    if (!atlas) { check(false, "readback of a missing atlas"); return p; }
    SDL_Texture* prev = SDL_GetRenderTarget(ren);
    SDL_SetRenderTarget(ren, atlas);
    check(SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, p.data(), kW * 4) == 0,
          std::string("readback: ") + SDL_GetError());
    SDL_SetRenderTarget(ren, prev);
    return p;
}

// The fixture: three animated sequences and one per-colour static texture.
struct Fixture {
    std::vector<std::vector<SDL_Texture*>> sequences;   // frame textures
    std::vector<SDL_Rect> rects;
    std::vector<SDL_Texture*> staticPerSlot;             // player-colour variants
    SDL_Rect staticRect{40, 30, 9, 7};
    SDL_Texture* shared = nullptr;                       // the same in every colour atlas
    SDL_Rect sharedRect{20, 2, 7, 7};
    std::vector<tak::render::AnimatedRegion> regions;
};

Fixture makeFixture() {
    Fixture f;
    // A: looping, 3 frames, 8x6. B: non-looping, 2 frames, 5x7 (expires).
    // C: 4 frames, 6x5; one frame LINEAR (exercises the flush protection), one
    // frame a different size (copied scaled into the rect, as in GameView).
    f.rects = {{2, 2, 8, 6}, {12, 2, 5, 7}, {2, 12, 6, 5}};
    f.sequences.push_back({makeTexture(8, 6, 1), makeTexture(8, 6, 2), makeTexture(8, 6, 3)});
    f.sequences.push_back({makeTexture(5, 7, 4), makeTexture(5, 7, 5)});
    f.sequences.push_back({makeTexture(6, 5, 6), makeTexture(6, 5, 7, SDL_ScaleModeLinear),
                           makeTexture(3, 5, 8), makeTexture(6, 5, 9)});
    for (int slot = 0; slot < 5; ++slot) f.staticPerSlot.push_back(makeTexture(9, 7, 100 + slot));
    f.shared = makeTexture(7, 7, 200);
    for (size_t i = 0; i < f.rects.size(); ++i) f.regions.push_back({f.rects[i], &f.sequences[i], 0});
    return f;
}

SDL_Texture* newAtlas() {
    SDL_Texture* a = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, kW, kH);
    SDL_SetTextureBlendMode(a, SDL_BLENDMODE_BLEND);
    return a;
}

void clearTarget(SDL_Texture* atlas) {
    SDL_SetRenderTarget(ren, atlas);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
    SDL_RenderClear(ren);
}

// GameView::atlasFor + paintAtlasContents: complete paint of one colour atlas.
SDL_Texture* createAtlas(const Fixture& f, int slot, tak::render::AtlasPaintCounts* counts = nullptr) {
    SDL_Texture* atlas = newAtlas();
    clearTarget(atlas);
    {
        tak::render::AtlasPainter painter(ren, counts);
        painter.copy(f.shared, f.sharedRect, nullptr);
        painter.copy(f.staticPerSlot[size_t(slot)], f.staticRect, nullptr);
        for (const auto& r : f.regions) tak::render::paintAnimatedRegion(painter, r);
        painter.finish();
    }
    SDL_SetRenderTarget(ren, nullptr);
    return atlas;
}

// GameView::atlasFor with another complete atlas: copy it whole, then clear and
// repaint only the per-colour texture and every animated region.
SDL_Texture* createAtlasFrom(const Fixture& f, int slot, SDL_Texture* from,
                             tak::render::AtlasPaintCounts* counts = nullptr) {
    SDL_Texture* atlas = newAtlas();
    clearTarget(atlas);
    {
        tak::render::AtlasPainter painter(ren, counts);
        painter.duplicate(from);
        painter.clear(f.staticRect);
        painter.copy(f.staticPerSlot[size_t(slot)], f.staticRect, nullptr);
        for (const auto& r : f.regions) tak::render::paintAnimatedRegion(painter, r);
        painter.finish();
    }
    SDL_SetRenderTarget(ren, nullptr);
    return atlas;
}

// The scheme this workstream replaced, kept here as the comparison reference:
// a new atlas paints every texture with its source blend mode; then, whenever any
// sequence changed (or an atlas was created), every animated region of every
// atlas is cleared and repainted with blend NONE, one flush per region.
void legacyPaintRect(SDL_Texture* src, const SDL_Rect& d) {
    int w = 0, h = 0;
    if (SDL_QueryTexture(src, nullptr, nullptr, &w, &h) != 0 || w <= 0 || h <= 0) return;
    SDL_RenderCopy(ren, src, nullptr, &d);
    const SDL_Rect edges[8][2] = {
        {{0, 0, w, 1}, {d.x, d.y - 1, d.w, 1}}, {{0, h - 1, w, 1}, {d.x, d.y + d.h, d.w, 1}},
        {{0, 0, 1, h}, {d.x - 1, d.y, 1, d.h}}, {{w - 1, 0, 1, h}, {d.x + d.w, d.y, 1, d.h}},
        {{0, 0, 1, 1}, {d.x - 1, d.y - 1, 1, 1}}, {{w - 1, 0, 1, 1}, {d.x + d.w, d.y - 1, 1, 1}},
        {{0, h - 1, 1, 1}, {d.x - 1, d.y + d.h, 1, 1}}, {{w - 1, h - 1, 1, 1}, {d.x + d.w, d.y + d.h, 1, 1}}};
    SDL_ScaleMode scale; SDL_GetTextureScaleMode(src, &scale);
    SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);
    for (const auto& e : edges) SDL_RenderCopy(ren, src, &e[0], &e[1]);
    SDL_RenderFlush(ren);
    SDL_SetTextureScaleMode(src, scale);
}
SDL_Texture* legacyCreate(const Fixture& f, int slot) {
    SDL_Texture* atlas = newAtlas();
    clearTarget(atlas);
    legacyPaintRect(f.shared, f.sharedRect);
    legacyPaintRect(f.staticPerSlot[size_t(slot)], f.staticRect);
    for (const auto& r : f.regions)
        if (r.frame < r.frames->size()) legacyPaintRect((*r.frames)[r.frame], r.rect);
    SDL_SetRenderTarget(ren, nullptr);
    return atlas;
}
void legacyRepaintAll(const Fixture& f, const std::vector<SDL_Texture*>& atlases) {
    for (SDL_Texture* atlas : atlases) {
        SDL_SetRenderTarget(ren, atlas);
        for (const auto& r : f.regions) {
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
            const SDL_Rect padded{r.rect.x - 1, r.rect.y - 1, r.rect.w + 2, r.rect.h + 2};
            SDL_RenderFillRect(ren, &padded);
            if (r.frame >= r.frames->size()) continue;
            SDL_Texture* src = (*r.frames)[r.frame];
            SDL_BlendMode b; SDL_GetTextureBlendMode(src, &b);
            SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
            legacyPaintRect(src, r.rect);
            SDL_SetTextureBlendMode(src, b);
        }
    }
    SDL_SetRenderTarget(ren, nullptr);
}

// Independent CPU expectation for one animated region of a read-back atlas: the
// frame's texels (only when the frame has the rect's size), a gutter repeating its
// edge texels, or transparent content AND gutter once expired.
void checkRegionOracle(const Pixels& atlas, const tak::render::AnimatedRegion& r,
                       const std::vector<Pixels>& frameBytes, const std::string& what) {
    const SDL_Rect& d = r.rect;
    const bool expired = r.frame >= r.frames->size();
    int fw = 0, fh = 0;
    if (!expired) SDL_QueryTexture((*r.frames)[r.frame], nullptr, nullptr, &fw, &fh);
    if (!expired && (fw != d.w || fh != d.h)) return;   // scaled frame: compared by reference only
    for (int y = d.y - 1; y <= d.y + d.h; ++y)
        for (int x = d.x - 1; x <= d.x + d.w; ++x) {
            const uint8_t* got = &atlas[(size_t(y) * kW + size_t(x)) * 4];
            uint8_t want[4] = {0, 0, 0, 0};
            if (!expired) {
                const int sx = std::clamp(x - d.x, 0, d.w - 1), sy = std::clamp(y - d.y, 0, d.h - 1);
                std::memcpy(want, &frameBytes[r.frame][(size_t(sy) * d.w + size_t(sx)) * 4], 4);
            }
            if (std::memcmp(got, want, 4)) {
                check(false, what + ": region texel (" + std::to_string(x) + "," + std::to_string(y) +
                             ") frame " + std::to_string(r.frame));
                return;
            }
        }
}

}  // namespace

int main() {
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* window = SDL_CreateWindow("animated atlas test", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
    if (!window) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!ren) ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { std::fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(ren, &info);
    std::printf("renderer: %s\n", info.name);

    Fixture f = makeFixture();
    std::vector<std::vector<Pixels>> frameBytes;
    for (auto& seq : f.sequences) {
        frameBytes.emplace_back();
        for (SDL_Texture* t : seq) frameBytes.back().push_back(textureBytes(t));
    }

    tak::render::GenerationCounter rendererGen, assetGen;
    tak::render::AnimatedAtlasTracker tracker;
    std::vector<SDL_Texture*> atlases, legacy;
    for (int slot = 0; slot < 2; ++slot) {
        tak::render::AtlasPaintCounts counts;
        atlases.push_back(createAtlas(f, slot, &counts));
        check(counts.flushes == 0, "creation with NEAREST sources needs no flush");
        tracker.created(size_t(slot), rendererGen.current(), assetGen.current(), f.regions);
        legacy.push_back(legacyCreate(f, slot));
    }
    legacyRepaintAll(f, legacy);   // legacy: atlas creation set the repaint flag

    // Frame schedule per step: A loops every step, B runs once then expires, C
    // changes every third step and passes through the LINEAR and odd-size frames.
    auto framesAt = [&](int step) {
        std::array<size_t, 3> fr{};
        fr[0] = size_t(step) % 3;
        fr[1] = step < 4 ? size_t(step / 2) : 2;   // 0,0,1,1, then expired (2 == size)
        fr[2] = size_t(step / 3) % 4;
        return fr;
    };

    auto updateAll = [&](tak::render::AtlasPaintCounts& counts, size_t& painted) {
        tak::render::AtlasPainter painter(ren, &counts);
        for (size_t slot = 0; slot < atlases.size(); ++slot) {
            if (!atlases[slot]) continue;
            if (tracker.current(slot, rendererGen.current(), assetGen.current(), f.regions)) continue;
            SDL_SetRenderTarget(ren, atlases[slot]);
            painted += tracker.update(slot, rendererGen.current(), assetGen.current(), f.regions, painter);
        }
        painter.finish();
        SDL_SetRenderTarget(ren, nullptr);
    };

    auto compareAll = [&](const std::string& when, bool withLegacy) {
        for (size_t slot = 0; slot < atlases.size(); ++slot) {
            if (!atlases[slot]) continue;
            const Pixels got = readAtlas(atlases[slot]);
            SDL_Texture* ref = createAtlas(f, int(slot));
            check(got == readAtlas(ref), when + ": atlas " + std::to_string(slot) + " == complete repaint");
            SDL_DestroyTexture(ref);
            if (withLegacy && slot < legacy.size() && legacy[slot])
                check(got == readAtlas(legacy[slot]), when + ": atlas " + std::to_string(slot) + " == previous scheme");
            for (size_t i = 0; i < f.regions.size(); ++i)
                checkRegionOracle(got, f.regions[i], frameBytes[i], when + " atlas " + std::to_string(slot));
        }
    };

    std::array<size_t, 3> last = framesAt(0);
    bool sawLinearFlush = false, sawExpiry = false;
    for (int step = 1; step <= 14; ++step) {
        const auto fr = framesAt(step);
        bool anyChanged = false;
        size_t changed = 0;
        for (size_t i = 0; i < 3; ++i) {
            f.regions[i].frame = fr[i];
            if (fr[i] != last[i]) { ++changed; anyChanged = true; }
        }
        sawExpiry |= fr[1] == 2 && last[1] != 2;
        const std::string when = "step " + std::to_string(step);

        // Late atlas creation: a third colour slot appears after B expired, painted
        // completely; a fourth starts as a copy of atlas 0 while atlas 0 samples
        // LINEAR (Bilinear Filtering on), which the copy must not pick up.
        const size_t existing = atlases.size();
        if (step == 7) {
            atlases.push_back(createAtlas(f, 2));
            tracker.created(2, rendererGen.current(), assetGen.current(), f.regions);
            legacy.push_back(legacyCreate(f, 2));
        }
        if (step == 10) {
            SDL_SetTextureScaleMode(atlases[0], SDL_ScaleModeLinear);
            tak::render::AtlasPaintCounts created;
            atlases.push_back(createAtlasFrom(f, 3, atlases[0], &created));
            SDL_ScaleMode mode = SDL_ScaleModeNearest;
            SDL_GetTextureScaleMode(atlases[0], &mode);
            check(mode == SDL_ScaleModeLinear, "copied-from atlas keeps its LINEAR mode");
            check(created.flushes == 2, "copying a LINEAR atlas flushes before and after");
            check(created.copies < 9 * (f.regions.size() + 3), "a copied atlas repaints only what differs");
            SDL_SetTextureScaleMode(atlases[0], SDL_ScaleModeNearest);
            tracker.created(3, rendererGen.current(), assetGen.current(), f.regions);
            legacy.push_back(legacyCreate(f, 3));
        }
        tak::render::AtlasPaintCounts counts;
        size_t painted = 0;
        updateAll(counts, painted);
        // Only the regions whose frame changed, once per atlas that existed before.
        check(painted == changed * existing,
              when + ": painted " + std::to_string(painted) + " regions, expected " +
              std::to_string(changed * existing));
        check(counts.copies <= painted * 9, when + ": no copies beyond the repainted regions");
        const bool linearPainted = fr[2] == 1 && last[2] != 1;
        check(counts.flushes == (linearPainted ? 2u : 0u),
              when + ": flushes " + std::to_string(counts.flushes) + " (only around a LINEAR source)");
        sawLinearFlush |= linearPainted;
        SDL_ScaleMode linearNow = SDL_ScaleModeNearest;
        SDL_GetTextureScaleMode(f.sequences[2][1], &linearNow);
        check(linearNow == SDL_ScaleModeLinear, when + ": a LINEAR source's scale mode is restored");
        if (anyChanged || step == 7 || step == 10) legacyRepaintAll(f, legacy);
        compareAll(when, true);
        last = fr;
    }
    check(sawLinearFlush && sawExpiry, "schedule covered the LINEAR source and the expiry");

    // An unchanged frame paints nothing and switches to no atlas.
    {
        tak::render::AtlasPaintCounts counts;
        size_t painted = 0;
        updateAll(counts, painted);
        check(painted == 0 && counts.copies == 0 && counts.regions == 0, "unchanged frames paint nothing");
    }

    // A late atlas under the previous scheme: animated regions painted with the
    // source blend until the next repaint pass. The new atlas is painted as a
    // repaint would be from the start (documented: the old transient frame differs).
    {
        SDL_Texture* oldLate = legacyCreate(f, 4);
        SDL_Texture* newLate = createAtlas(f, 4);
        const bool transientDiffers = readAtlas(oldLate) != readAtlas(newLate);
        std::vector<SDL_Texture*> one{oldLate};
        legacyRepaintAll(f, one);
        check(readAtlas(oldLate) == readAtlas(newLate), "late atlas equals the previous scheme after its repaint");
        std::printf("late atlas: previous scheme's creation frame %s\n",
                    transientDiffers ? "differed (source-blend animated regions)" : "matched");
        SDL_DestroyTexture(oldLate);
        SDL_DestroyTexture(newLate);
    }

    // Renderer reset: every atlas dies; recreated ones are complete, and a slot the
    // tracker still lists under the old generation is repainted entirely.
    {
        for (auto*& a : atlases) { SDL_DestroyTexture(a); a = nullptr; }
        tracker.clear();
        rendererGen.bump();
        f.regions[0].frame = 1; f.regions[2].frame = 3;
        atlases[0] = createAtlas(f, 0);
        tracker.created(0, rendererGen.current(), assetGen.current(), f.regions);
        compareAll("after reset (recreated)", false);
        // Slot 1: an atlas whose record is from another generation (or absent).
        atlases[1] = newAtlas();
        clearTarget(atlases[1]);
        {
            tak::render::AtlasPainter painter(ren);
            painter.copy(f.shared, f.sharedRect, nullptr);
            painter.copy(f.staticPerSlot[1], f.staticRect, nullptr);
            painter.finish();
        }
        SDL_SetRenderTarget(ren, nullptr);
        tak::render::AtlasPaintCounts counts;
        size_t painted = 0;
        atlases.resize(2);
        updateAll(counts, painted);
        check(painted == f.regions.size(), "unknown atlas: every region repainted");
        compareAll("after reset (unknown slot)", false);
        // An asset-generation change invalidates every record.
        assetGen.bump();
        painted = 0;
        updateAll(counts, painted);
        check(painted == f.regions.size() * 2, "asset generation change repaints every region");
        compareAll("after asset generation change", false);
        check(tracker.known(0, rendererGen.current(), assetGen.current()), "updated slot is known");
        check(!tracker.known(0, rendererGen.current(), assetGen.current() + 1), "another generation is not known");
        tracker.forget(0);
        check(tracker.painted(0, 0) == tak::render::AnimatedAtlasTracker::kUnpainted, "forget clears a slot");
        check(!tracker.known(0, rendererGen.current(), assetGen.current()), "a forgotten slot is not known");
        check(!tracker.known(7, rendererGen.current(), assetGen.current()), "a never-seen slot is not known");
    }

    for (auto* a : atlases) if (a) SDL_DestroyTexture(a);
    for (auto* a : legacy) if (a) SDL_DestroyTexture(a);
    for (auto& seq : f.sequences) for (auto* t : seq) SDL_DestroyTexture(t);
    for (auto* t : f.staticPerSlot) SDL_DestroyTexture(t);
    SDL_DestroyTexture(f.shared);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("PASS: animated atlas (incremental == complete repaint == previous scheme)\n");
    return 0;
}
