#pragma once

// Colour-atlas painting for model textures and per-atlas tracking of the frame
// each animated region holds (render optimization workstream B).
//
// Every model texture has one rect in each player-colour atlas, with a one-texel
// clamped gutter inside a two-texel pad (GameView::buildAtlasLayout). Animated
// sequences select one frame for ALL colour atlases from the game tick; a region's
// content is therefore a pure function of that frame value:
//   * frame < frames.size(): the frame texture copied with blend NONE, plus a
//     gutter of its own edge texels;
//   * otherwise (a non-looping sequence that has expired): transparent, gutter
//     included.
// So a region is current exactly when it was last painted with the same frame
// value, under the same renderer and asset generations. AnimatedAtlasTracker keeps
// that record per atlas and repaints only regions whose frame changed, instead of
// repainting every animated region of every atlas whenever any sequence advanced.
//
// Presentation state only: nothing here feeds the simulation.

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "client/rendertelemetry.h"

namespace tak::render {

struct AtlasPaintCounts {
    uint64_t regions = 0;   // animated regions repainted (clear + optional copy)
    uint64_t pixels = 0;    // padded pixels of those regions
    uint64_t copies = 0;    // texture copies (body + eight gutter strips)
    uint64_t flushes = 0;   // explicit SDL_RenderFlush calls
};

// Queues atlas copies without a flush per region.
//
// SDL records a copy's blend mode in its command queue when the copy is queued,
// but reads the texture's SCALE mode when the queue RUNS (GL texture parameters;
// the software renderer's scaler). The gutter strips must sample NEAREST, so a
// source that is not already NEAREST is switched once -- after a flush, so draws of
// it queued earlier still run with its own mode -- and switched back in finish(),
// after one flush that runs everything queued meanwhile. Model textures are created
// NEAREST and never change, so in practice no flush is needed at all; the protection
// stays for any source that is not. (The previous per-region code flushed every
// region; its body copy also ran NEAREST on GL, since the mode is applied to the GL
// texture immediately and the queued copy ran after it.)
class AtlasPainter {
public:
    explicit AtlasPainter(SDL_Renderer* renderer, AtlasPaintCounts* counts = nullptr)
        : renderer_(renderer), counts_(counts) {}
    ~AtlasPainter() { finish(); }
    AtlasPainter(const AtlasPainter&) = delete;
    AtlasPainter& operator=(const AtlasPainter&) = delete;

    // Clear a content rect AND its one-texel gutter to transparent (0,0,0,0).
    void clear(const SDL_Rect& content);
    // Copy src into `content` plus a clamped one-texel gutter. `blend` overrides the
    // source's blend mode for these copies (restored as soon as they are queued);
    // nullptr uses the source's own mode. A source whose size cannot be queried is
    // skipped, as before.
    void copy(SDL_Texture* src, const SDL_Rect& content, const SDL_BlendMode* blend);
    // Copy the whole of `src` (an atlas of the same size) 1:1 onto the whole target
    // with blend NONE: pixels, gutters and padding exactly. Lets a new colour atlas
    // start from an existing one and repaint only the regions that differ.
    void duplicate(SDL_Texture* src);
    // Run what is queued if a source's scale mode was changed, then restore it.
    void finish();

private:
    void sampleNearest(SDL_Texture* src);
    SDL_Renderer* renderer_;
    AtlasPaintCounts* counts_;
    std::vector<std::pair<SDL_Texture*, SDL_ScaleMode>> restore_;
};

// One animated model texture's rect in every colour atlas, and its frame now.
struct AnimatedRegion {
    SDL_Rect rect{};                                  // content rect (gutter around it)
    const std::vector<SDL_Texture*>* frames = nullptr; // the sequence's frame textures
    size_t frame = 0;                                 // selected frame; >= size: expired
};

// Paint one animated region into the current render target (clear + frame copy).
void paintAnimatedRegion(AtlasPainter& painter, const AnimatedRegion& region);

// Which frame each animated region of each colour atlas holds. The region list is
// indexed identically for every atlas of one asset generation: GameView builds it
// from the animated-texture registry, which only changes when textures reload (a
// new asset generation, and the atlases are destroyed then anyway).
class AnimatedAtlasTracker {
public:
    static constexpr size_t kUnpainted = SIZE_MAX;

    // Atlas `slot` was just created and completely painted with these frames.
    void created(size_t slot, Generation renderer, Generation asset,
                 std::span<const AnimatedRegion> regions);
    // Repaint, into the CURRENT render target (atlas `slot`), every region whose
    // painted frame differs from regions[i].frame. A slot never recorded, or recorded
    // under another generation, has every region repainted. Returns regions painted.
    size_t update(size_t slot, Generation renderer, Generation asset,
                  std::span<const AnimatedRegion> regions, AtlasPainter& painter);
    // True when the slot has a record under these generations (its painted frames
    // are known, whatever they are).
    bool known(size_t slot, Generation renderer, Generation asset) const {
        return slot < slots_.size() && slots_[slot].renderer == renderer &&
               slots_[slot].asset == asset && renderer != kNoGeneration;
    }
    // True when update() would paint nothing for this slot.
    bool current(size_t slot, Generation renderer, Generation asset,
                 std::span<const AnimatedRegion> regions) const;
    void forget(size_t slot) { if (slot < slots_.size()) slots_[slot] = {}; }
    void clear() { slots_.clear(); }
    // The frame region i of `slot` holds, or kUnpainted.
    size_t painted(size_t slot, size_t region) const {
        return slot < slots_.size() && region < slots_[slot].painted.size()
            ? slots_[slot].painted[region] : kUnpainted;
    }

private:
    struct Slot {
        Generation renderer = kNoGeneration;
        Generation asset = kNoGeneration;
        std::vector<size_t> painted;
    };
    std::vector<Slot> slots_;
};

}  // namespace tak::render
