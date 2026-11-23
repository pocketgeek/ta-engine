#include "client/mapview.h"

#include "client/gpuvram.h"
#include "client/rendertelemetry.h"
#include "client/dev.h"
#include "hpi/hpi.h"           // tak::hpi::Vfs::read (ctor / reload)
#include "tnt/mapgen.h"        // "~gen1~" random-map ids -> procedural map

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace {
uint64_t steadyNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Debug: TAK_TERRAIN_SECTION_BUDGET_MIB=N sets the decoded-section budget's high
// mark (low = 7/8 of it), TAK_TERRAIN_SECTION_MIN_IDLE_MS=N the working-set window;
// release builds always use the defaults.
void applySectionBudget(tak::terrain::Compositor& comp) {
    size_t high = MapView::kSectionCpuHigh, low = MapView::kSectionCpuLow;
    uint64_t minIdle = MapView::kSectionMinIdleMs;
    if (const char* v = tak::devEnv("TAK_TERRAIN_SECTION_BUDGET_MIB")) {
        high = size_t(std::max(1L, std::atol(v))) << 20;
        low = high - high / 8;
    }
    if (const char* v = tak::devEnv("TAK_TERRAIN_SECTION_MIN_IDLE_MS")) minIdle = uint64_t(std::max(0L, std::atol(v)));
    comp.setBudget(high, low, minIdle);
}
// Debug: TAK_TERRAIN_MIP_BUDGET_MIB=N (low = 7/8 of it), TAK_TERRAIN_MIP_IDLE_MS=N.
void applyMipBudget(tak::TerrainChunks& chunks) {
    size_t high = tak::TerrainChunks::kMipHigh, low = tak::TerrainChunks::kMipLow;
    uint64_t idle = tak::TerrainChunks::kMipIdleMs;
    if (const char* v = tak::devEnv("TAK_TERRAIN_MIP_BUDGET_MIB")) {
        high = size_t(std::max(1L, std::atol(v))) << 20;
        low = high - high / 8;
    }
    if (const char* v = tak::devEnv("TAK_TERRAIN_MIP_IDLE_MS")) idle = uint64_t(std::max(0L, std::atol(v)));
    chunks.setMipBudget(high, low, idle);
}
// Debug: TAK_TERRAIN_SCHEDULE=legacy restores the pre-K terrain work policy (FIFO, no
// cancellation, no calm gate, plain LRU, separate upload caps) in the same binary; a
// comma list of fifo,nocancel,nogate,nostandin,nocoord switches single parts off,
// nearby switches the experimental leading-edge prefetch on, gateall applies the calm
// gate's fast-motion rule at every level (the rejected K5 scope; default: level 0 only)
// and stormlevel0 limits its jump-storm rule to level 0 (the rejected K6 scope; default:
// every level).
tak::terrainwork::Params scheduleParams() {
    tak::terrainwork::Params p;
    const char* v = tak::devEnv("TAK_TERRAIN_SCHEDULE");
    if (!v) return p;
    std::string list = v;
    for (size_t at = 0; at <= list.size();) {
        const size_t end = std::min(list.find(',', at), list.size());
        const std::string token = list.substr(at, end - at);
        if (token == "legacy") p = tak::terrainwork::Params::legacy();
        else if (token == "fifo") p.prioritize = false;
        else if (token == "nocancel") p.cancel = false;
        else if (token == "nogate") p.calmGate = false;
        else if (token == "gateall") p.calmGateMaxLevel = 5;
        else if (token == "stormlevel0") p.stormGateMaxLevel = 0;
        else if (token == "nostandin") p.protectStandins = false;
        else if (token == "nocoord") p.coordinate = false;
        else if (token == "nearby") p.nearby = true;
        at = end + 1;
    }
    return p;
}
}

tak::tnt::Map MapView::genOrLoad(const tak::hpi::Vfs& vfs, const std::string& mapPath) {
    if (tak::mapgen::isGeneratedMapId(mapPath))
        return tak::mapgen::generate(tak::mapgen::parseMapId(mapPath), vfs).map;
    return tak::tnt::Map::load(vfs.read(mapPath), mapPath);
}

MapView::MapView(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, const std::string& mapPath)
    : ren_(ren), map_(genOrLoad(vfs, mapPath)), comp_(vfs) {
    applySectionBudget(comp_);
    applyMipBudget(chunks_);
    chunks_.setParams(scheduleParams());
    uploadBudget_.coordinate = chunks_.params().coordinate;
    secWorker_ = std::thread([this] { sectionWorkerLoop(); });
    queueAllSections();
}

MapView::MapView(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, tak::tnt::Map map)
    : ren_(ren), map_(std::move(map)), comp_(vfs) {
    applySectionBudget(comp_);
    applyMipBudget(chunks_);
    chunks_.setParams(scheduleParams());
    uploadBudget_.coordinate = chunks_.params().coordinate;
    secWorker_ = std::thread([this] { sectionWorkerLoop(); });
    queueAllSections();
}

MapView::~MapView() {
    invalidateRenderTargets();
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
    dropFreeSections();
}

void MapView::reload(const tak::hpi::Vfs& vfs, const std::string& mapPath) {
    invalidateRenderTargets();
    quiesce();
    comp_.clear();
    for (auto& [k, s] : sections_) if (s.tex) gpuvram::destroy(s.tex);
    sections_.clear();
    sectionBytes_ = 0;
    dropFreeSections();
    wanted_.clear();
    batchKeys_.clear();
    tileBatch_.clear();
    apronBatch_.clear();
    tileBatchDirty_ = true;
    builtZoom_ = -1;   // force a rebuild against the new map
    map_ = genOrLoad(vfs, mapPath);
    queueAllSections();
}

void MapView::quiesce() {
    // Both workers can read the VFS; callers also use this before remounting it.
    chunks_.resetSource();
    // The section decoder also reads map_, which is about to be swapped.
    {
        std::unique_lock<std::mutex> lk(secMu_);
        decodeQueue_.clear();
        secCv_.wait(lk, [this] { return !secBusy_; });
        decoded_.clear();      // stale decodes queued against the OLD map
        secPending_.clear();
    }
}

void MapView::queueAllSections() {
    // The map references a handful of section JPGs (Two Castles: 27). Collect
    // the unique keys and hand them to the decode worker; each becomes one GPU
    // texture. Deduped via secPending_.
    std::set<uint32_t> keys(map_.tileKeys.begin(), map_.tileKeys.end());
    std::lock_guard<std::mutex> lk(secMu_);
    for (uint32_t k : keys)
        if (secPending_.insert(k).second) decodeQueue_.push_back({k, false});
    secCv_.notify_all();
}

void MapView::tilesEdited(int bx,int by,int width,int height) {
    const int x0=std::clamp(bx,0,map_.blocksX),y0=std::clamp(by,0,map_.blocksY);
    const int x1=std::clamp(bx+width,0,map_.blocksX),y1=std::clamp(by+height,0,map_.blocksY);
    if(x0>=x1 || y0>=y1)return;
    chunks_.invalidate(x0,y0,x1,y1);tileBatchDirty_=true;
    std::set<uint32_t> keys;
    for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)keys.insert(map_.tileKeys[size_t(y)*map_.blocksX+x]);
    std::lock_guard lock(secMu_);
    for(uint32_t key:keys)if(secPending_.insert(key).second)decodeQueue_.push_back({key,false});
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
    // view in one dimension — otherwise the view runs off the map edges.
    if (mapW > 0 && mapH > 0) clampZoom(winW, winH);
    float maxX = mapW - winW / zoom_, maxY = mapH - winH / zoom_;
    offX_ = maxX <= 0 ? maxX / 2 : std::clamp(offX_, 0.0f, maxX);
    offY_ = maxY <= 0 ? maxY / 2 : std::clamp(offY_, 0.0f, maxY);
}

void MapView::ensureChunks(int winW, int winH) {
    // Sections stay decoded in the Compositor (TerrainChunks composes from the same
    // cache); draw() uploads the ones the fallback needs.
    clampOffset(winW, winH);
    uploadReadySections();
}

void MapView::finishChunks() {
    // Screenshot path: block until every section is decoded (see header).
    {
        std::unique_lock<std::mutex> lk(secMu_);
        secCv_.wait(lk, [this] { return decodeQueue_.empty() && !secBusy_; });
    }
    pinNext_ = true;
    uploadReadySections();
}

size_t MapView::shedGpu(size_t need) {
    size_t freed = 0;
    while (!gpuvram::wouldFit(need) && freeSectionBytes_) {   // kept textures first
        const size_t before = freeSectionBytes_;
        dropFreeSection();
        freed += before - freeSectionBytes_;
    }
    while (!gpuvram::wouldFit(need)) {   // duplicates of chunk terrain go next
        auto oldest = oldestIdleSection();
        if (oldest == sections_.end()) break;
        freed += size_t(oldest->second.w) * oldest->second.h * 4;
        evictSection(oldest, false);
    }
    return freed + chunks_.shed(need);
}

void MapView::rebuildTileBatch(int winW, int winH) {
    for (auto& [t, v] : tileBatch_) v.clear();   // keep per-texture capacity
    for (auto& [t, v] : apronBatch_) v.clear();
    wanted_.clear();
    batchKeys_.clear();
    underlayTiles_ = 0;
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
            if(chunksEnabled_ && chunks_.covers(bx,by)) {bx=(bx/32+1)*32-1;continue;}
            size_t b = size_t(by) * mapW + bx;
            auto si = sections_.find(map_.tileKeys[b]);
            if (si == sections_.end()) { wanted_.push_back(map_.tileKeys[b]); ++underlayTiles_; continue; }  // underlay shows
            if (!si->second.tex) continue;   // absent JPG
            const Section& s = si->second;
            batchKeys_.push_back(map_.tileKeys[b]);
            // Tile crop within the section (retail wraps col/row by the JPG size).
            int sx = (map_.tileCols[b] * kBlock) % std::max(s.w, 1);
            int sy = (map_.tileRows[b] * kBlock) % std::max(s.h, 1);
            // Half-texel inset: bilinear at the quad edge then samples exactly the
            // tile's own edge texel, never the neighbour -- no bleed without a baked
            // gutter. Clamp for the rare non-32-multiple section. Nearest sampling has
            // always used these same UVs, so it stays pixel-identical.
            float u0 = (sx + 0.5f) / s.w, v0 = (sy + 0.5f) / s.h;
            float u1 = (std::min(sx + kBlock, s.w) - 0.5f) / s.w;
            float v1 = (std::min(sy + kBlock, s.h) - 0.5f) / s.h;
            // Linear sampling would still stop at an inset edge, leaving a hard line
            // every 32 texels. Where the map neighbour is the very next crop of the
            // same section -- maps are mostly laid out that way -- its texels ARE the
            // right neighbours: sample to the true edge and the filter crosses the
            // boundary exactly as it does inside a chunk. Elsewhere see the aprons.
            bool join[4] = {};   // left, top, right, bottom
            if (bilinear_) {
                const auto next = [&](int nx, int ny, int dx, int dy) {
                    if (nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) return false;
                    const size_t n = size_t(ny) * mapW + nx;
                    return map_.tileKeys[n] == map_.tileKeys[b] &&
                           (map_.tileCols[n] * kBlock) % std::max(s.w, 1) == sx + dx &&
                           (map_.tileRows[n] * kBlock) % std::max(s.h, 1) == sy + dy;
                };
                if ((join[0] = sx > 0 && next(bx - 1, by, -kBlock, 0))) u0 = float(sx) / s.w;
                if ((join[1] = sy > 0 && next(bx, by - 1, 0, -kBlock))) v0 = float(sy) / s.h;
                if ((join[2] = sx + kBlock < s.w && next(bx + 1, by, kBlock, 0))) u1 = float(sx + kBlock) / s.w;
                if ((join[3] = sy + kBlock < s.h && next(bx, by + 1, 0, kBlock))) v1 = float(sy + kBlock) / s.h;
            }
            float x0 = float(std::lround((bx * kBlock - offX_) * zoom_));
            float x1 = float(std::lround(((bx + 1) * kBlock - offX_) * zoom_));
            auto& vb = tileBatch_[s.tex];
            SDL_Vertex tl{{x0, y0}, white, {u0, v0}};
            SDL_Vertex tr{{x1, y0}, white, {u1, v0}};
            SDL_Vertex br{{x1, y1}, white, {u1, v1}};
            SDL_Vertex bl{{x0, y1}, white, {u0, v1}};
            vb.push_back(tl); vb.push_back(tr); vb.push_back(br);
            vb.push_back(tl); vb.push_back(br); vb.push_back(bl);
            // Below 1x (only the Cartographer filters there) a half texel is under half
            // a pixel and the fades are invisible, while a zoomed-out fallback view
            // would quadruple the tile vertices -- skip them.
            if (!bilinear_ || zoom_ < 1.0f) continue;
            // Aprons: across a boundary to an unrelated crop, the neighbour's edge
            // texel row/column is laid over this tile's outer half texel, fading from
            // half strength at the boundary to nothing at this tile's edge-texel
            // centre. Over this tile's own (clamped) edge texel that IS the bilinear
            // blend of the two -- the half texel on the far side gets the matching
            // apron from the neighbour's own pass, or is a chunk, which already
            // filters across with its gutter. Only corners (one texel square, where
            // the diagonal tile is ignored) are approximate.
            const float h = 0.5f * zoom_;
            const int nb[4][2] = {{bx - 1, by}, {bx, by - 1}, {bx + 1, by}, {bx, by + 1}};
            for (int side = 0; side < 4; ++side) {
                const int nx = nb[side][0], ny = nb[side][1];
                if (join[side] || nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) continue;
                const size_t n = size_t(ny) * mapW + nx;
                auto ni = sections_.find(map_.tileKeys[n]);
                if (ni == sections_.end()) { wanted_.push_back(map_.tileKeys[n]); continue; }
                if (!ni->second.tex) continue;
                const Section& ns = ni->second;
                batchKeys_.push_back(map_.tileKeys[n]);
                const float nsx = float((map_.tileCols[n] * kBlock) % std::max(ns.w, 1));
                const float nsy = float((map_.tileRows[n] * kBlock) % std::max(ns.h, 1));
                // The neighbour's texel row/column facing this tile, along its inset span.
                const float nu0 = (nsx + 0.5f) / ns.w, nv0 = (nsy + 0.5f) / ns.h;
                const float nu1 = (std::min(nsx + kBlock, float(ns.w)) - 0.5f) / ns.w;
                const float nv1 = (std::min(nsy + kBlock, float(ns.h)) - 0.5f) / ns.h;
                const SDL_Color edge{255, 255, 255, 128}, fade{255, 255, 255, 0};
                SDL_Vertex q[4];   // boundary pair first, then the inner pair
                if (side == 0)      q[0] = {{x0, y0}, edge, {nu1, nv0}}, q[1] = {{x0, y1}, edge, {nu1, nv1}},
                                    q[2] = {{x0 + h, y1}, fade, {nu1, nv1}}, q[3] = {{x0 + h, y0}, fade, {nu1, nv0}};
                else if (side == 2) q[0] = {{x1, y0}, edge, {nu0, nv0}}, q[1] = {{x1, y1}, edge, {nu0, nv1}},
                                    q[2] = {{x1 - h, y1}, fade, {nu0, nv1}}, q[3] = {{x1 - h, y0}, fade, {nu0, nv0}};
                else if (side == 1) q[0] = {{x0, y0}, edge, {nu0, nv1}}, q[1] = {{x1, y0}, edge, {nu1, nv1}},
                                    q[2] = {{x1, y0 + h}, fade, {nu1, nv1}}, q[3] = {{x0, y0 + h}, fade, {nu0, nv1}};
                else                q[0] = {{x0, y1}, edge, {nu0, nv0}}, q[1] = {{x1, y1}, edge, {nu1, nv0}},
                                    q[2] = {{x1, y1 - h}, fade, {nu1, nv0}}, q[3] = {{x0, y1 - h}, fade, {nu0, nv0}};
                auto& ab = apronBatch_[ns.tex];
                ab.push_back(q[0]); ab.push_back(q[1]); ab.push_back(q[2]);
                ab.push_back(q[0]); ab.push_back(q[2]); ab.push_back(q[3]);
            }
        }
    }
    for (auto* keys : {&wanted_, &batchKeys_}) {
        std::sort(keys->begin(), keys->end());
        keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
    }
    builtOffX_ = offX_; builtOffY_ = offY_; builtZoom_ = zoom_;
    builtW_ = winW; builtH_ = winH;
    tileBatchDirty_ = false;

}

void MapView::draw(int winW, int winH, int viewH) {
    clampOffset(winW, viewH < 0 ? winH : viewH);

    // Underlay first: stretch the overview across the whole map's screen rect. Tiles
    // draw on top at full detail; not-yet-uploaded sections fall back to this.
    if (underlay_) {
        int mapW = map_.blocksX * kBlock, mapH = map_.blocksY * kBlock;
        int ux0 = int(std::lround((0 - offX_) * zoom_)), uy0 = int(std::lround((0 - offY_) * zoom_));
        int ux1 = int(std::lround((mapW - offX_) * zoom_)), uy1 = int(std::lround((mapH - offY_) * zoom_));
        SDL_Rect udst{ux0, uy0, ux1 - ux0, uy1 - uy0};
        tak::render::countLogicalDraw();
        SDL_RenderCopy(ren_, underlay_, nullptr, &udst);
    }

    // Cached map chunks follow the camera with a few image copies. Only missing
    // chunks use the original tile geometry while the worker prepares them.
    ++frame_;
    pinning_=std::exchange(pinNext_,false);
    comp_.setClockMs(nowMs());
    comp_.trim();   // over the CPU budget only while pins or the working set held more
    chunksEnabled_=!tak::devFlag("TAK_TERRAIN_CACHE_OFF");
    uploadBudget_.beginFrame(pinning_);
    if(chunksEnabled_) {
        chunks_.prepare(ren_,map_,offX_,offY_,zoom_,winW,winH,bilinear_,nowMs(),uploadBudget_);
#ifndef NDEBUG
        // Debug (capture runs): TAK_TERRAIN_SETTLE=1 waits until every visible chunk is
        // ready at its wanted level, so a capture shows finished terrain whatever order
        // or speed the work had (bounded: after one 10 s wait it gives up for good).
        // The calm gate is lifted while settling: this compares finished terrain, and the
        // loop runs on the owner's (possibly fixed) clock.
        static bool settleGaveUp=false;
        if(!settleGaveUp && tak::devFlag("TAK_TERRAIN_SETTLE"))
            for(const uint64_t start=SDL_GetTicks64();;) {
                if(chunks_.params().calmGate) {auto p=chunks_.params();p.calmGate=false;chunks_.setParams(p);settleRestoreGate_=true;}
                const auto settled=chunks_.stats();
                if(settled.visible && settled.ready==settled.visible)break;
                if(SDL_GetTicks64()-start>=10000) {
                    std::fprintf(stderr,"TAK_TERRAIN_SETTLE: terrain did not settle; giving up\n");
                    settleGaveUp=true;break;
                }
                SDL_Delay(1);uploadBudget_.beginFrame(pinning_);
                chunks_.prepare(ren_,map_,offX_,offY_,zoom_,winW,winH,bilinear_,nowMs(),uploadBudget_);
                chunks_.uploadDeferred(ren_,uploadBudget_);
            }
        if(settleRestoreGate_) {auto p=chunks_.params();p.calmGate=true;chunks_.setParams(p);settleRestoreGate_=false;}
#endif
        const auto revision=chunks_.stats().revision;
        if(revision!=chunkRevision_) {tileBatchDirty_=true;chunkRevision_=revision;}
        if(chunks_.draw(ren_,offX_,offY_,zoom_)) {
            tileBatch_.clear();apronBatch_.clear();wanted_.clear();batchKeys_.clear();underlayTiles_=0;
            chunks_.uploadDeferred(ren_,uploadBudget_);
            tileBatchDirty_=true;trimSections();return;
        }
    } else {chunks_.clear();tileBatchDirty_=true;}
    const bool changed=tileBatchDirty_ || offX_ != builtOffX_ || offY_ != builtOffY_ ||
        zoom_ != builtZoom_ || winW != builtW_ || winH != builtH_;
    // Mark what the batch samples as in use BEFORE uploading, so making room for a
    // missing section never evicts one this frame draws.
    const auto stamp=[&] {
        const uint64_t now=nowMs();
        for(uint32_t key:batchKeys_)if(auto i=sections_.find(key);i!=sections_.end()) {i->second.frame=frame_;i->second.usedMs=now;}
    };
    if(changed)rebuildTileBatch(winW,winH);
    stamp();
    // Same frame: the tiles that just came into the fallback get their sections now,
    // not the frame after, so a fast pan shows terrain rather than the underlay.
    if(!wanted_.empty() && uploadWantedSections()) {rebuildTileBatch(winW,winH);stamp();}

    for (auto& [tex, verts] : tileBatch_)
        if (tex && !verts.empty())
            tileSubmit_.draw(ren_, tex, verts, !tak::devFlag("TAK_TERRAIN_SDL_SUBMIT"));
    // Linear-only blends across unrelated tile boundaries, over every base tile.
    for (auto& [tex, verts] : apronBatch_)
        if (tex && !verts.empty()) {
            tak::render::countSdlGeometry(verts.size(), verts.size() * sizeof(SDL_Vertex));
            SDL_RenderGeometry(ren_, tex, verts.data(), int(verts.size()), nullptr, 0);
        }
    // Chunk results that are not hole fills (upgrades over a stand-in, nearby and kept
    // work) upload after the fallback's sections, within what the frame has left.
    if (chunksEnabled_) chunks_.uploadDeferred(ren_, uploadBudget_);
    trimSections();
}

void MapView::invalidateRenderTargets() {
    chunks_.clear();tileBatchDirty_=true;
}

void MapView::setBilinear(bool b) {
    if (b == bilinear_) return;
    bilinear_ = b;
    tileBatchDirty_ = true;   // the fallback tile UVs depend on it (rebuildTileBatch)
    for (auto& [k, s] : sections_)
        if (s.tex) SDL_SetTextureScaleMode(s.tex, b ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}

void MapView::setZoomSpeed(float m) { zoomSpeed_ = std::clamp(m, 0.25f, 4.0f); }

void MapView::uploadReadySections() {
    std::vector<std::pair<uint32_t, bool>> ready;
    {
        std::lock_guard<std::mutex> lk(secMu_);
        ready.swap(decoded_);
    }
    // Decoded images stay in the compositor until used (or evicted under its CPU
    // budget); only an absent JPG needs noting here, so it is never retried.
    for (const auto& [key, found] : ready)
        if (!found && !sections_.count(key)) sections_[key] = {};
}

bool MapView::uploadWantedSections() {
    // Chunks off: the fallback IS the terrain, so only the GPU cap bounds it.
    const size_t budget = chunksEnabled_ ? kSectionBudget : SIZE_MAX;
    const int perFrame = pinning_ ? INT_MAX : kSectionUploadsPerFrame;
    const uint64_t now = nowMs();
    int n = 0;
    std::vector<uint32_t> decode;
    for (uint32_t key : wanted_) {
        if (sections_.count(key)) continue;
        // Never decode on the render thread in normal play: what the compositor does
        // not hold (not decoded yet, or evicted under its CPU budget) is requested from
        // the decode worker, ahead of the prefetch. finishChunks() frames (captures,
        // the briefing backdrop) decode here instead, so they show complete terrain.
        bool absent = false;
        tak::terrain::SectionRef img = comp_.cachedSection(key, map_.stockTerrain, &absent);   // pinned until uploaded
        if (absent) { sections_[key] = {}; continue; }
        if (!img && !pinning_) { decode.push_back(key); continue; }
        if (n >= perFrame) continue;   // the rest next frame; the underlay shows meanwhile
        if (!img) try { img = comp_.sectionImage(key, map_.stockTerrain); } catch (const std::exception&) {
            sections_[key] = {};
            continue;
        }
        const size_t size = size_t(img->width) * img->height * 4;
        // A section fills a visible hole: an urgent upload of the frame's terrain budget,
        // shared with the chunk images (finishChunks frames: unlimited). The rest waits
        // for the next frame; the underlay shows meanwhile.
        if (!uploadBudget_.admit(tak::terrainwork::UploadBudget::Section, true, size)) continue;
        // A kept texture of this size takes it with no new GPU memory. Otherwise kept
        // textures, then idle sections, make room; the budget never refuses one needed
        // now. Under the GPU cap, off-screen chunks go next: terrain on screen comes first.
        SDL_Texture* t = nullptr;
        auto spare = freeSections_.find({img->width, img->height});
        if (spare != freeSections_.end() && !spare->second.empty()) {
            t = spare->second.back();
            spare->second.pop_back();
            freeSectionBytes_ -= size;
            ++sectionStats_.reuses;
        } else {
            while (sectionBytes_ + freeSectionBytes_ + size > budget || !gpuvram::wouldFit(size)) {
                if (dropFreeSection()) continue;
                auto oldest = oldestIdleSection();
                if (oldest == sections_.end()) break;
                evictSection(oldest, false);
            }
            if (!gpuvram::wouldFit(size)) chunks_.shed(size);
            if (!gpuvram::wouldFit(size) || gpuvram::blocked()) continue;
        }
        const uint64_t uploadStart = steadyNs();
        if (!t) {
            t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img->width, img->height);
            if (!t) { gpuvram::noteFail(); continue; }
        }
        SDL_UpdateTexture(t, nullptr, img->rgba.data(), img->width * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);   // opaque texels; the aprons fade by vertex alpha
        SDL_SetTextureScaleMode(t, bilinear_ ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        const uint64_t uploadNs = steadyNs() - uploadStart;   // CPU submission only
        sectionStats_.uploadNs += uploadNs;
        uploadBudget_.record(tak::terrainwork::UploadBudget::Section, size, uploadNs);
        ++sectionStats_.uploads;
        sectionStats_.uploadBytes += size;
        tak::render::countTextureUpload(size);
        sections_[key] = {t, img->width, img->height, frame_, now};
        sectionBytes_ += size;
        ++n;
        if (tak::devFlag("TAK_TERRAINLOG"))
            std::fprintf(stderr, "terrain: section %08x %dx%d uploaded; %zu resident (%zu MiB), gpu=%zuMiB\n",
                         key, img->width, img->height, sections_.size(), sectionBytes_ >> 20,
                         gpuvram::bytes() >> 20);
    }
    if (!decode.empty()) {
        // Needed on screen now: decode these ahead of the load-time queue. A key whose
        // decode was dropped (quiesce) is queued again.
        std::lock_guard<std::mutex> lk(secMu_);
        for (uint32_t k : decode) {
            if (!secPending_.insert(k).second) {
                auto it = std::find_if(decodeQueue_.begin(), decodeQueue_.end(),
                                       [k](const DecodeRequest& r) { return r.key == k; });
                if (it == decodeQueue_.end()) continue;   // in flight
                if (it == decodeQueue_.begin()) { it->demand = true; continue; }
                decodeQueue_.erase(it);
            }
            decodeQueue_.push_front({k, true});
        }
        secCv_.notify_all();
    }
    return n > 0;
}

uint64_t MapView::nowMs() const { return clockSet_ ? clockMs_ : SDL_GetTicks64(); }

std::map<uint32_t, MapView::Section>::iterator MapView::oldestIdleSection() {
    auto oldest = sections_.end();
    for (auto i = sections_.begin(); i != sections_.end(); ++i)
        if (i->second.tex && i->second.frame != frame_ &&
            (oldest == sections_.end() || i->second.usedMs < oldest->second.usedMs)) oldest = i;
    return oldest;
}

void MapView::evictSection(std::map<uint32_t, Section>::iterator it, bool pool) {
    const size_t size = size_t(it->second.w) * it->second.h * 4;
    sectionBytes_ -= size;
    tileBatch_.erase(it->second.tex);    // emptied per-texture slots (kept for capacity)
    apronBatch_.erase(it->second.tex);
    const size_t budget = chunksEnabled_ ? kSectionBudget : SIZE_MAX;
    if (pool && sectionBytes_ + freeSectionBytes_ + size <= budget && !gpuvram::overCap()) {
        freeSections_[{it->second.w, it->second.h}].push_back(it->second.tex);
        freeSectionBytes_ += size;
    } else {
        gpuvram::destroy(it->second.tex);
    }
    sections_.erase(it);
}

bool MapView::dropFreeSection() {
    for (auto& [dims, textures] : freeSections_)
        if (!textures.empty()) {
            gpuvram::destroy(textures.back());
            textures.pop_back();
            freeSectionBytes_ -= size_t(dims.first) * dims.second * 4;
            return true;
        }
    return false;
}

void MapView::dropFreeSections() {
    while (dropFreeSection()) {}
    freeSections_.clear();
}

void MapView::trimSections() {
    if (pinning_ || (!sectionBytes_ && !freeSectionBytes_)) return;
    const uint64_t now = nowMs();
    for (auto i = sections_.begin(); i != sections_.end();) {
        auto next = std::next(i);
        if (i->second.tex && i->second.frame != frame_ && now - i->second.usedMs >= kSectionIdleMs)
            evictSection(i);
        i = next;
    }
    // Over the idle budget or a lowered GPU limit: kept textures, then anything not
    // drawn this frame, oldest first.
    const size_t budget = chunksEnabled_ ? kSectionBudget : SIZE_MAX;
    while (sectionBytes_ + freeSectionBytes_ > budget || gpuvram::overCap()) {
        if (dropFreeSection()) continue;
        auto oldest = oldestIdleSection();
        if (oldest == sections_.end()) break;
        evictSection(oldest, false);
    }
}

void MapView::sectionWorkerLoop() {
    std::unique_lock<std::mutex> lk(secMu_);
    for (;;) {
        secCv_.wait(lk, [this] { return secStop_ || !decodeQueue_.empty(); });
        if (secStop_) return;
        const DecodeRequest request = decodeQueue_.front();
        decodeQueue_.pop_front();
        secBusy_ = true;
        lk.unlock();
        // Decode into the Compositor cache (the heavy JPEG work, once per section while
        // it stays within the CPU budget). A prefetch only fills free budget: it never
        // makes the cache evict, so a map larger than the budget is not decoded whole
        // and then trimmed; what a view needs arrives as a demand request instead.
        bool found = true;
        const bool cached = comp_.cachedSection(request.key, map_.stockTerrain) != nullptr;
        if (!cached && !request.demand && !comp_.admitsPrefetch(0)) {
            prefetchSkipped_.fetch_add(1, std::memory_order_relaxed);
        } else if (!cached) {
            const uint64_t decodeStart = steadyNs();
            try { comp_.sectionImage(request.key, map_.stockTerrain); } catch (const std::exception&) { found = false; }
            sectionDecodeNs_.fetch_add(steadyNs() - decodeStart, std::memory_order_relaxed);
            sectionDecodes_.fetch_add(1, std::memory_order_relaxed);
        }
        lk.lock();
        secBusy_ = false;
        secPending_.erase(request.key);
        decoded_.push_back({request.key, found});
        secCv_.notify_all();   // reload()/finishChunks() may be waiting
    }
}
