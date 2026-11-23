#include "client/animatedatlas.h"

#include <algorithm>

namespace tak::render {

void AtlasPainter::clear(const SDL_Rect& content) {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    const SDL_Rect padded{content.x - 1, content.y - 1, content.w + 2, content.h + 2};
    SDL_RenderFillRect(renderer_, &padded);
    if (counts_) {
        ++counts_->regions;
        counts_->pixels += uint64_t(padded.w) * uint64_t(padded.h);
    }
}

void AtlasPainter::copy(SDL_Texture* src, const SDL_Rect& d, const SDL_BlendMode* blend) {
    int w = 0, h = 0;
    if (!src || SDL_QueryTexture(src, nullptr, nullptr, &w, &h) != 0 || w <= 0 || h <= 0) return;
    sampleNearest(src);
    SDL_BlendMode previous = SDL_BLENDMODE_NONE;
    if (blend) {
        SDL_GetTextureBlendMode(src, &previous);
        SDL_SetTextureBlendMode(src, *blend);
    }
    SDL_RenderCopy(renderer_, src, nullptr, &d);
    const SDL_Rect edges[8][2] = {
        {{0, 0, w, 1}, {d.x, d.y - 1, d.w, 1}}, {{0, h - 1, w, 1}, {d.x, d.y + d.h, d.w, 1}},
        {{0, 0, 1, h}, {d.x - 1, d.y, 1, d.h}}, {{w - 1, 0, 1, h}, {d.x + d.w, d.y, 1, d.h}},
        {{0, 0, 1, 1}, {d.x - 1, d.y - 1, 1, 1}}, {{w - 1, 0, 1, 1}, {d.x + d.w, d.y - 1, 1, 1}},
        {{0, h - 1, 1, 1}, {d.x - 1, d.y + d.h, 1, 1}}, {{w - 1, h - 1, 1, 1}, {d.x + d.w, d.y + d.h, 1, 1}}};
    for (const auto& e : edges) SDL_RenderCopy(renderer_, src, &e[0], &e[1]);
    // The blend mode was recorded with each queued copy; restoring it now is safe.
    if (blend) SDL_SetTextureBlendMode(src, previous);
    if (counts_) counts_->copies += 9;
}

void AtlasPainter::sampleNearest(SDL_Texture* src) {
    SDL_ScaleMode scale = SDL_ScaleModeNearest;
    SDL_GetTextureScaleMode(src, &scale);
    if (scale == SDL_ScaleModeNearest) return;
    // Draws of src already queued must run with its own mode.
    SDL_RenderFlush(renderer_);
    if (counts_) ++counts_->flushes;
    restore_.emplace_back(src, scale);
    SDL_SetTextureScaleMode(src, SDL_ScaleModeNearest);
}

void AtlasPainter::duplicate(SDL_Texture* src) {
    if (!src) return;
    sampleNearest(src);   // a colour atlas is LINEAR while Bilinear Filtering is on
    SDL_BlendMode previous = SDL_BLENDMODE_NONE;
    SDL_GetTextureBlendMode(src, &previous);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(renderer_, src, nullptr, nullptr);
    SDL_SetTextureBlendMode(src, previous);
    if (counts_) ++counts_->copies;
}

void AtlasPainter::finish() {
    if (restore_.empty()) return;
    SDL_RenderFlush(renderer_);   // queued copies must run before the modes change back
    if (counts_) ++counts_->flushes;
    // Restore in reverse so a source listed twice ends with its original mode.
    for (auto it = restore_.rbegin(); it != restore_.rend(); ++it)
        SDL_SetTextureScaleMode(it->first, it->second);
    restore_.clear();
}

void paintAnimatedRegion(AtlasPainter& painter, const AnimatedRegion& region) {
    painter.clear(region.rect);
    if (!region.frames || region.frame >= region.frames->size()) return;   // expired
    SDL_Texture* frame = (*region.frames)[region.frame];
    if (!frame) return;
    const SDL_BlendMode none = SDL_BLENDMODE_NONE;
    painter.copy(frame, region.rect, &none);
}

void AnimatedAtlasTracker::created(size_t slot, Generation renderer, Generation asset,
                                   std::span<const AnimatedRegion> regions) {
    if (slots_.size() <= slot) slots_.resize(slot + 1);
    Slot& s = slots_[slot];
    s.renderer = renderer;
    s.asset = asset;
    s.painted.resize(regions.size());
    for (size_t i = 0; i < regions.size(); ++i) s.painted[i] = regions[i].frame;
}

bool AnimatedAtlasTracker::current(size_t slot, Generation renderer, Generation asset,
                                   std::span<const AnimatedRegion> regions) const {
    if (slot >= slots_.size()) return regions.empty();
    const Slot& s = slots_[slot];
    if (s.renderer != renderer || s.asset != asset || s.painted.size() != regions.size())
        return regions.empty();
    for (size_t i = 0; i < regions.size(); ++i)
        if (s.painted[i] != regions[i].frame) return false;
    return true;
}

size_t AnimatedAtlasTracker::update(size_t slot, Generation renderer, Generation asset,
                                    std::span<const AnimatedRegion> regions,
                                    AtlasPainter& painter) {
    if (slots_.size() <= slot) slots_.resize(slot + 1);
    Slot& s = slots_[slot];
    if (s.renderer != renderer || s.asset != asset || s.painted.size() != regions.size()) {
        // Unknown content: every region is repainted.
        s.renderer = renderer;
        s.asset = asset;
        s.painted.assign(regions.size(), kUnpainted);
    }
    size_t painted = 0;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (s.painted[i] == regions[i].frame) continue;
        paintAnimatedRegion(painter, regions[i]);
        s.painted[i] = regions[i].frame;
        ++painted;
    }
    return painted;
}

}  // namespace tak::render
