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
    secWorker_ = std::thread([this] { sectionWorkerLoop(); });
    queueAllSections();
}

MapView::MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, ta::tnt::Map map)
    : ren_(ren), map_(std::move(map)), comp_(vfs) {
    secWorker_ = std::thread([this] { sectionWorkerLoop(); });
    queueAllSections();
}

MapView::~MapView() {
    {
        std::lock_guard<std::mutex> lk(secMu_);
        secStop_ = true;
    }
    secCv_.notify_all();
    if (secWorker_.joinable()) secWorker_.join();
    // Free the section textures: the renderer outlives the session, so skipping
    // this leaked the map's terrain textures (and gpuvram budget) on every
    // menu->game->menu loop.
    for (auto& [k, s] : sections_) if (s.tex) gpuvram::destroy(s.tex);
}

void MapView::reload(const ta::hpi::Vfs& vfs, const std::string& mapPath) {
    // Quiesce the decode worker first: it reads map_, which is about to be swapped.
    {
        std::unique_lock<std::mutex> lk(secMu_);
        decodeQueue_.clear();
        secCv_.wait(lk, [this] { return !secBusy_; });
        decoded_.clear();      // stale decodes queued against the OLD map
        secPending_.clear();
    }
    for (auto& [k, s] : sections_) if (s.tex) gpuvram::destroy(s.tex);
    sections_.clear();
    tileBatch_.clear();
    tileBatchDirty_ = true;
    builtZoom_ = -1;   // force a rebuild against the new map
    map_ = genOrLoad(vfs, mapPath);
    queueAllSections();
}

void MapView::queueAllSections() {
    // The map references a handful of section JPGs (Two Castles: 27). Collect
    // the unique keys and hand them to the decode worker; each becomes one GPU
    // texture. Deduped via secPending_.
    std::set<uint32_t> keys(map_.tileKeys.begin(), map_.tileKeys.end());
    std::lock_guard<std::mutex> lk(secMu_);
    for (uint32_t k : keys)
        if (secPending_.insert(k).second) decodeQueue_.push_back(k);
    secCv_.notify_all();
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
    // (Kept name: the call sites are unchanged.) Adopt any sections the worker
    // decoded since last frame; that's all the per-frame texture work now --
    // there is no per-view streaming/eviction. The map's whole section set
    // (~7 MiB) is resident once decoded, resolution-independent.
    clampOffset(winW, winH);
    uploadReadySections();
}

void MapView::finishChunks() {
    // Screenshot path: block until every section is decoded AND uploaded.
    {
        std::unique_lock<std::mutex> lk(secMu_);
        secCv_.wait(lk, [this] { return decodeQueue_.empty() && !secBusy_; });
    }
    for (;;) {
        uploadReadySections();
        std::lock_guard<std::mutex> lk(secMu_);
        if (decoded_.empty()) break;
    }
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
            auto si = sections_.find(map_.tileKeys[b]);
            if (si == sections_.end() || !si->second.tex) continue;  // underlay shows
            const Section& s = si->second;
            // Tile crop within the section (retail wraps col/row by the JPG size).
            int sx = (map_.tileCols[b] * kBlock) % std::max(s.w, 1);
            int sy = (map_.tileRows[b] * kBlock) % std::max(s.h, 1);
            // Half-texel inset: bilinear at the quad edge then samples exactly the
            // tile's own edge texel, never the neighbour -- seam-free without a
            // baked gutter. Clamp for the rare non-32-multiple section.
            float u0 = (sx + 0.5f) / s.w, v0 = (sy + 0.5f) / s.h;
            float u1 = (std::min(sx + kBlock, s.w) - 0.5f) / s.w;
            float v1 = (std::min(sy + kBlock, s.h) - 0.5f) / s.h;
            float x0 = float(std::lround((bx * kBlock - offX_) * zoom_));
            float x1 = float(std::lround(((bx + 1) * kBlock - offX_) * zoom_));
            auto& vb = tileBatch_[s.tex];
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

    // Underlay first: stretch the overview across the whole map's screen rect. Tiles
    // draw on top at full detail; not-yet-uploaded sections fall back to this.
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
    for (auto& [k, s] : sections_)
        if (s.tex) SDL_SetTextureScaleMode(s.tex, b ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

void MapView::setZoomSpeed(float m) { zoomSpeed_ = std::clamp(m, 0.25f, 4.0f); }

void MapView::uploadReadySections() {
    std::vector<uint32_t> ready;
    {
        std::lock_guard<std::mutex> lk(secMu_);
        ready.swap(decoded_);
    }
    // A whole map is ~27 sections; upload a few per frame so a cold start never
    // hitches, and never start the VRAM-exhaustion storm. Deferred keys stay in
    // decoded_ (re-queued below) and upload once there's room.
    constexpr size_t kUploadBudget = 3;
    size_t n = 0;
    std::vector<uint32_t> deferred;
    for (uint32_t key : ready) {
        if (sections_.count(key)) continue;   // already uploaded
        const ta::jpeg::Image* img = nullptr;
        try { img = &comp_.sectionImage(key); } catch (const std::exception&) {
            sections_[key] = {};   // absent JPG: remember so we never retry it
            continue;
        }
        if (n >= kUploadBudget) { deferred.push_back(key); continue; }
        if (gpuvram::blocked() ||
            !gpuvram::wouldFit(size_t(img->width) * img->height * 4)) {
            deferred.push_back(key); continue;
        }
        SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, img->width, img->height);
        if (!t) { gpuvram::noteFail(); deferred.push_back(key); continue; }
        SDL_UpdateTexture(t, nullptr, img->rgba.data(), img->width * 4);
        SDL_SetTextureScaleMode(t, bilinear_ ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        sections_[key] = {t, img->width, img->height};
        tileBatchDirty_ = true;   // a new section can fill in visible tiles
        ++n;
        static const bool kLog = std::getenv("TA_TERRAINLOG") != nullptr;
        if (kLog)
            std::fprintf(stderr, "terrain: section %08x %dx%d uploaded; %zu total, gpu=%zuMiB\n",
                         key, img->width, img->height, sections_.size(),
                         gpuvram::bytes() >> 20);
    }
    if (!deferred.empty()) {
        std::lock_guard<std::mutex> lk(secMu_);
        for (uint32_t k : deferred) decoded_.push_back(k);
    }
}

void MapView::sectionWorkerLoop() {
    std::unique_lock<std::mutex> lk(secMu_);
    for (;;) {
        secCv_.wait(lk, [this] { return secStop_ || !decodeQueue_.empty(); });
        if (secStop_) return;
        uint32_t key = decodeQueue_.front();
        decodeQueue_.pop_front();
        secBusy_ = true;
        lk.unlock();
        // Decode into the Compositor cache (the heavy JPEG work, once per section).
        try { comp_.sectionImage(key); } catch (const std::exception&) { /* absent */ }
        lk.lock();
        secBusy_ = false;
        decoded_.push_back(key);
        secCv_.notify_all();   // reload()/finishChunks() may be waiting
    }
}
