#include "client/bigmapview.h"

#include "hpi/hpi.h"

#include <algorithm>
#include <cmath>

BigMapView::BigMapView(SDL_Renderer* ren, std::unique_ptr<tak::hpi::Vfs> vfs, const std::string& tntPath,
                       const std::string& world)
    : ren_(ren), vfs_(std::move(vfs)) {
    view_ = std::make_unique<MapView>(ren_, *vfs_, tntPath);
    loadFeatures(world);
}

BigMapView::BigMapView(SDL_Renderer* ren, std::unique_ptr<tak::hpi::Vfs> vfs, tak::tnt::Map map,
                       const std::string& world)
    : ren_(ren), vfs_(std::move(vfs)) {
    view_ = std::make_unique<MapView>(ren_, *vfs_, std::move(map));
    loadFeatures(world);
}

BigMapView::~BigMapView() {
    features_.quiesce();
    for (auto& [index, sprite] : sprites_)
        if (sprite.tex) SDL_DestroyTexture(sprite.tex);
}

float BigMapView::worldW() const { return float(view_->map().blocksX * 32); }
float BigMapView::worldH() const { return float(view_->map().blocksY * 32); }

// Every feature the map places, decoded once (frame 0, as Cartographer draws them).
// A name no feature file of this world defines has no art and is left out.
void BigMapView::loadFeatures(const std::string& world) {
    features_.scan(*vfs_, world.empty() ? "aramon" : world);
    const auto& names = view_->map().featureNames;
    for (size_t i = 0; i < names.size() && i < 0xFFFA; ++i) {
        const cart::FeatureRef* ref = features_.byName(names[i]);
        const cart::FeatSprite* sp = ref ? features_.sprite(*vfs_, *ref) : nullptr;
        if (!sp || sp->w <= 0 || sp->h <= 0) continue;
        SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, sp->w, sp->h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, sp->rgba.data(), sp->w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        sprites_[uint16_t(i)] = {t, sp->w, sp->h, sp->xoff, sp->yoff};
    }
    const auto& m = view_->map();
    if (m.features.size() != size_t(m.width) * size_t(m.height)) return;
    for (int cz = 0; cz < m.height; ++cz)
        for (int cx = 0; cx < m.width; ++cx) {
            const auto it = sprites_.find(m.features[size_t(cz) * m.width + cx]);
            if (it != sprites_.end()) placed_.push_back({cx * 16.0f, cz * 16.0f, &it->second});
        }
}

SDL_FRect BigMapView::draw(const SDL_FRect& area) {
    const float mapW = worldW(), mapH = worldH();
    if (mapW <= 0 || mapH <= 0 || area.w <= 0 || area.h <= 0) return {};
    const float fit = std::min(area.w / mapW, area.h / mapH);
    const int w = std::max(1, int(mapW * fit)), h = std::max(1, int(mapH * fit));
    const SDL_Rect vp{int(area.x + (area.w - float(w)) / 2), int(area.y + (area.h - float(h)) / 2), w, h};
    // MapView draws from the viewport's corner and never zooms out past filling it.
    const float zoom = std::max(float(w) / mapW, float(h) / mapH);
    view_->setZoom(zoom);
    view_->setOffset(0, 0);
    SDL_RenderSetViewport(ren_, &vp);
    view_->ensureChunks(w, h);
    view_->draw(w, h);
    // Features over the terrain, each anchored at its cell as in the game.
    for (const Placed& p : placed_) {
        const Sprite& s = *p.sprite;
        const SDL_FRect dst{(p.x - s.xoff) * zoom, (p.y - s.yoff) * zoom, s.w * zoom, s.h * zoom};
        SDL_RenderCopyF(ren_, s.tex, nullptr, &dst);
    }
    SDL_RenderSetViewport(ren_, nullptr);
    return {float(vp.x), float(vp.y), float(w), float(h)};
}
