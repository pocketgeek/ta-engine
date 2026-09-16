#include "client/mapview.h"

#include "client/gpuvram.h"
#include "hpi/hpi.h"           // ta::hpi::Vfs::read (ctor / reload)
#include "tnt/mapgen.h"        // "~gen1~" random-map ids -> procedural map

#include <algorithm>
#include <cmath>

ta::tnt::Map MapView::genOrLoad(const ta::hpi::Vfs& vfs, const std::string& mapPath) {
    if (ta::mapgen::isGeneratedMapId(mapPath))
        return ta::mapgen::generate(ta::mapgen::decodeMapId(mapPath), vfs).map;
    return ta::tnt::Map::load(vfs.read(mapPath), mapPath);
}

MapView::MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, const std::string& mapPath)
    : ren_(ren), map_(genOrLoad(vfs, mapPath)), comp_(vfs) {
    buildAtlas();
}

MapView::MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, ta::tnt::Map map)
    : ren_(ren), map_(std::move(map)), comp_(vfs) {
    buildAtlas();
}

MapView::~MapView() { destroyAtlas(); }

void MapView::destroyAtlas() {
    // The renderer outlives the session, so skipping this leaks the map's terrain
    // texture (and its gpuvram budget) on every menu->game->menu loop.
    if (atlas_) gpuvram::destroy(atlas_);
    atlas_ = nullptr;
    atlasCols_ = atlasW_ = atlasH_ = 0;
    tileBatch_.clear();
}

void MapView::buildAtlas() {
    destroyAtlas();
    const int n = map_.numTiles;
    if (n <= 0) return;

    // Near-square grid, so the atlas stays well inside any sane max-texture
    // limit: even a 4096-tile map lands at 64x64 tiles = 2048x2048 px.
    atlasCols_ = int(std::ceil(std::sqrt(double(n))));
    int rows = (n + atlasCols_ - 1) / atlasCols_;
    atlasW_ = atlasCols_ * kBlock;
    atlasH_ = rows * kBlock;

    // Expand every tile through the palette into one RGBA image.
    std::vector<uint8_t> px(size_t(atlasW_) * atlasH_ * 4, 0);
    const auto& pal = comp_.palette();
    for (int t = 0; t < n; ++t) {
        const uint8_t* src = map_.tile(t);
        if (!src) continue;
        int ox = (t % atlasCols_) * kBlock, oy = (t / atlasCols_) * kBlock;
        for (int y = 0; y < kBlock; ++y) {
            uint8_t* dst = &px[(size_t(oy + y) * atlasW_ + ox) * 4];
            for (int x = 0; x < kBlock; ++x) {
                const uint8_t* c = pal.rgba[src[y * kBlock + x]];
                dst[x * 4 + 0] = c[0]; dst[x * 4 + 1] = c[1];
                dst[x * 4 + 2] = c[2]; dst[x * 4 + 3] = 255;
            }
        }
    }

    if (!gpuvram::wouldFit(px.size())) return;   // underlay carries the view
    atlas_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                             atlasW_, atlasH_);
    if (!atlas_) { gpuvram::noteFail(); atlasCols_ = atlasW_ = atlasH_ = 0; return; }
    SDL_UpdateTexture(atlas_, nullptr, px.data(), atlasW_ * 4);
    SDL_SetTextureScaleMode(atlas_, bilinear_ ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    tileBatchDirty_ = true;
}

void MapView::reload(const ta::hpi::Vfs& vfs, const std::string& mapPath) {
    map_ = genOrLoad(vfs, mapPath);
    builtZoom_ = -1;          // force a batch rebuild against the new map
    buildAtlas();             // also clears the batch and the old texture
}

void MapView::input(const SDL_Event& e) {
    if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
        offX_ -= e.motion.xrel / zoom_;
        offY_ -= e.motion.yrel / zoom_;
    } else if (e.type == SDL_MOUSEWHEEL) {
        // Half the old per-notch step (1.25/0.8): 1.25^0.5 in, its reciprocal out.
        // zoomSpeed_ is an exponent so in/out stay reciprocal and 1.0 == the base.
        float f = std::pow(e.wheel.y > 0 ? 1.118f : 0.894f, zoomSpeed_);
        zoom_ = std::clamp(zoom_ * f, 0.05f, 4.0f);
    } else if (e.type == SDL_KEYDOWN) {
        float step = 200 / zoom_;
        switch (e.key.keysym.sym) {
            case SDLK_LEFT: offX_ -= step; break;
            case SDLK_RIGHT: offX_ += step; break;
            case SDLK_UP: offY_ -= step; break;
            case SDLK_DOWN: offY_ += step; break;
            case SDLK_EQUALS: case SDLK_PLUS: zoom_ = std::min(zoom_ * 1.25f, 4.0f); break;
            case SDLK_MINUS: zoom_ = std::max(zoom_ * 0.8f, 0.05f); break;
        }
    }
}

float MapView::minZoom(int winW, int winH) const {
    int mapW = map_.blocksX * 32, mapH = map_.blocksY * 32;
    return (mapW > 0 && mapH > 0) ? std::max(float(winW) / mapW, float(winH) / mapH) : 0.05f;
}

void MapView::clampZoom(int winW, int winH) { zoom_ = std::max(zoom_, minZoom(winW, winH)); }

void MapView::clampOffset(int winW, int winH) {
    int mapW = map_.blocksX * 32, mapH = map_.blocksY * 32;
    // Don't allow zooming out past the point where the map fills the
    // window in one dimension — otherwise the view runs off the map edges.
    if (mapW > 0 && mapH > 0) {
        float mz = std::max(float(winW) / mapW, float(winH) / mapH);
        if (zoom_ < mz) zoom_ = mz;
    }
    float maxX = mapW - winW / zoom_, maxY = mapH - winH / zoom_;
    offX_ = maxX <= 0 ? maxX / 2 : std::clamp(offX_, 0.0f, maxX);
    offY_ = maxY <= 0 ? maxY / 2 : std::clamp(offY_, 0.0f, maxY);
}

void MapView::ensureChunks(int winW, int winH) {
    // (Kept name: the call sites are unchanged.) There is no per-frame texture
    // work left -- the tile atlas is built once at map load and stays resident,
    // resolution-independent -- so this just keeps the view on the map.
    clampOffset(winW, winH);
}


void MapView::rebuildTileBatch(int winW, int winH) {
    for (auto& [t, v] : tileBatch_) v.clear();   // keep per-texture capacity
    const int mapW = map_.blocksX, mapH = map_.blocksY;
    // Visible block range, clamped to the map.
    int b0x = std::max(0, int(std::floor(offX_ / kBlock)));
    int b0y = std::max(0, int(std::floor(offY_ / kBlock)));
    int b1x = std::min(mapW - 1, int(std::floor((offX_ + winW / zoom_) / kBlock)));
    int b1y = std::min(mapH - 1, int(std::floor((offY_ + winH / zoom_) / kBlock)));
    const SDL_Color white{255, 255, 255, 255};
    for (int by = b0y; by <= b1y; ++by) {
        // Shared edges are computed from the WORLD edge (identical for adjacent
        // tiles), so neighbours abut at exactly the same integer pixel -- no gaps,
        // no overlap, matching the old integer-rounded chunk edges.
        float y0 = float(std::lround((by * kBlock - offY_) * zoom_));
        float y1 = float(std::lround(((by + 1) * kBlock - offY_) * zoom_));
        for (int bx = b0x; bx <= b1x; ++bx) {
            size_t b = size_t(by) * mapW + bx;
            int t = map_.tiles[b];
            if (!atlas_ || t < 0 || t >= map_.numTiles) continue;   // underlay shows
            int sx = (t % atlasCols_) * kBlock, sy = (t / atlasCols_) * kBlock;
            // Half-texel inset: bilinear at the quad edge then samples exactly the
            // tile's own edge texel, never the neighbour -- seam-free without a
            // baked gutter. Matters more here than with sections, because every
            // tile in the atlas has a different tile on all four sides.
            float u0 = (sx + 0.5f) / atlasW_, v0 = (sy + 0.5f) / atlasH_;
            float u1 = (sx + kBlock - 0.5f) / atlasW_;
            float v1 = (sy + kBlock - 0.5f) / atlasH_;
            float x0 = float(std::lround((bx * kBlock - offX_) * zoom_));
            float x1 = float(std::lround(((bx + 1) * kBlock - offX_) * zoom_));
            auto& vb = tileBatch_[atlas_];
            SDL_Vertex tl{{x0, y0}, white, {u0, v0}};
            SDL_Vertex tr{{x1, y0}, white, {u1, v0}};
            SDL_Vertex br{{x1, y1}, white, {u1, v1}};
            SDL_Vertex bl{{x0, y1}, white, {u0, v1}};
            vb.push_back(tl); vb.push_back(tr); vb.push_back(br);
            vb.push_back(tl); vb.push_back(br); vb.push_back(bl);
        }
    }
    builtOffX_ = offX_; builtOffY_ = offY_; builtZoom_ = zoom_;
    builtW_ = winW; builtH_ = winH;
    tileBatchDirty_ = false;
}

void MapView::draw(int winW, int winH) {
    clampOffset(winW, winH);

    // Underlay first: stretch the overview across the whole map's screen rect.
    // Tiles draw on top at full detail; if the atlas could not be uploaded (VRAM
    // pressure), this is what the player sees.
    if (underlay_) {
        int mapW = map_.blocksX * kBlock, mapH = map_.blocksY * kBlock;
        int ux0 = int(std::lround((0 - offX_) * zoom_)), uy0 = int(std::lround((0 - offY_) * zoom_));
        int ux1 = int(std::lround((mapW - offX_) * zoom_)), uy1 = int(std::lround((mapH - offY_) * zoom_));
        SDL_Rect udst{ux0, uy0, ux1 - ux0, uy1 - uy0};
        SDL_RenderCopy(ren_, underlay_, nullptr, &udst);
    }

    // Rebuild the tile-quad batch only when the view moved or a section uploaded
    // (idle spectating rebuilds nothing -- just re-submits the cached batch).
    if (tileBatchDirty_ || offX_ != builtOffX_ || offY_ != builtOffY_ ||
        zoom_ != builtZoom_ || winW != builtW_ || winH != builtH_)
        rebuildTileBatch(winW, winH);

    for (auto& [tex, verts] : tileBatch_)
        if (tex && !verts.empty())
            SDL_RenderGeometry(ren_, tex, verts.data(), int(verts.size()), nullptr, 0);
}

void MapView::setBilinear(bool b) {
    bilinear_ = b;
    if (atlas_) SDL_SetTextureScaleMode(atlas_, b ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

void MapView::setZoomSpeed(float m) { zoomSpeed_ = std::clamp(m, 0.25f, 4.0f); }


