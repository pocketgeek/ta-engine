#pragma once

// Terrain map view: pans/zooms a TNT map and composites its tile chunks on a
// background worker thread (TerrainChunks). Extracted from
// client/main.cpp; kept at global scope so its unqualified use sites there are
// unchanged.
//
// Lifetime: the Vfs passed to the ctor/reload MUST outlive the MapView — the
// terrain::Compositor borrows it by reference. (GameView declares its vfs_
// member before its mapView_ member for exactly this reason.)
// Owns a worker thread + mutex, so it is implicitly non-copyable/non-movable.

#include <SDL.h>

#include "terrain/terrain.h"   // tak::terrain::Compositor (by-value member)
#include "tnt/tnt.h"           // tak::tnt::Map (by-value member)
#include "client/geometrysubmit.h"
#include "client/terrainchunks.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tak::hpi { class Vfs; }

class MapView {
public:
    MapView(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, const std::string& mapPath);
    // Construct directly from an in-memory map (e.g. the editor's fresh/blank map).
    MapView(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, tak::tnt::Map map);
    ~MapView();

    // Swap in a different map (discarding cached chunk textures). The compositor's
    // decoded-tile cache is content-addressed by tile key, so it stays valid. Used at
    // game start so the render terrain matches the map the sim actually loaded.
    void reload(const tak::hpi::Vfs& vfs, const std::string& mapPath);

    void input(const SDL_Event& e);

    // The smallest zoom at which the map still fills the window in one dimension
    // (zooming out past this would run the view off the map edges). winW/winH are
    // the area the map is SEEN in: a game passes its world view (GameView::mapViewW/
    // mapViewH -- the window less the right panel and the bottom bar), the same size
    // it gives clampOffset, so the floor and the scroll limits agree.
    float minZoom(int winW, int winH) const;
    // Apply just the min-zoom floor (no offset change).
    void clampZoom(int winW, int winH);
    void clampOffset(int winW, int winH);

    // Per-frame upkeep before the render pass: clamp the view and take in sections the
    // decode worker has finished (they upload on demand in draw()).
    void ensureChunks(int winW, int winH);

    // Block until every queued section is decoded, and let the NEXT draw upload every
    // section its fallback needs with no per-frame limit, so that frame shows
    // complete terrain even where chunks are still baking. Sections that frame uses stay
    // resident while used; normal budgeting resumes after it. Screenshot paths and the
    // campaign briefing backdrop -- normal play never waits.
    void finishChunks();
    void quiesce(); // cancel pending reads before changing the VFS

    // Give GPU memory back for an essential allocation: drop sections and chunks not
    // drawn this frame, oldest first, until gpuvram::wouldFit(need). Returns bytes freed.
    size_t shedGpu(size_t need);

    // A low-res whole-map overview (one texel per 32px block), drawn UNDER the chunk
    // grid so a not-yet-composited chunk shows blurry terrain instead of a black
    // rectangle. The owner sets this (GameView's minimap texture); null = none.
    void setUnderlay(SDL_Texture* t) { underlay_ = t; }

    // Clock for section idle times (ms). Wall time unless the owner supplies its own,
    // e.g. the render study's fixed presentation clock, so which sections a frame has
    // resident does not depend on how fast the build runs.
    void setClockMs(uint64_t ms) { clockMs_ = ms; clockSet_ = true; }

    void invalidateRenderTargets();

    // Draws winW x winH; the scroll clamp uses viewH (default winH), the height the map
    // is SEEN in -- a game draws on under its opaque bottom bar but scrolls above it.
    void draw(int winW, int winH, int viewH = -1);

    // Linear terrain sampling (GameView's Texture Filtering = Everything while zoomed
    // in; the Cartographer always). Applies to already-built chunk and section textures
    // immediately and to every one built afterwards. Chunks carry a one-texel gutter of
    // real neighbouring terrain; fallback tiles sample to the true edge where the
    // neighbour is the adjoining crop of the same section, and blend an apron of the
    // neighbour's edge texels elsewhere (rebuildTileBatch), so neither shows seams.
    void setBilinear(bool b);

    float offX() const { return offX_; }
    float offY() const { return offY_; }
    float zoom() const { return zoom_; }
    tak::terrain::Compositor& compositor() { return comp_; }
    void setZoom(float z) { zoom_ = z; }
    void setOffset(float x, float y) { offX_ = x; offY_ = y; }
    void setZoomSpeed(float m);
    const tak::tnt::Map& map() const { return map_; }

    // --- Editing (Cartographer) ------------------------------------------------
    // Mutable terrain access: edit map().tileKeys/tileCols/tileRows/heights/
    // features, then call tilesEdited() so any newly-referenced section textures
    // decode+upload and the tile-quad batch rebuilds next frame.
    tak::tnt::Map& editMap() { return map_; }
    void tilesEdited() { chunks_.clear(); queueAllSections(); tileBatchDirty_ = true; }
    void tilesEdited(int bx,int by,int width,int height);
    tak::TerrainChunks::Stats chunkStats() const { return chunks_.stats(); }
    bool chunkCovers(int blockX, int blockY) const { return chunks_.covers(blockX, blockY); }   // tests
    void setMipBudget(size_t high, size_t low, uint64_t idleMs) { chunks_.setMipBudget(high, low, idleMs); }
    // Terrain work policy (workstream K; tests and debug: Params::legacy() = pre-K).
    void setScheduleParams(const tak::terrainwork::Params& p) { chunks_.setParams(p); uploadBudget_.coordinate = p.coordinate; }
    size_t sectionBytes() const { return sectionBytes_; }   // resident section textures
    // Fallback tiles the last draw() left to the underlay (their section was not
    // resident): visible terrain shown only as the blurry overview (workstream K).
    size_t underlayTiles() const { return underlayTiles_; }
    // Render telemetry for the section fallback (cumulative; the reader takes deltas).
    struct SectionStats {
        uint64_t uploads = 0, uploadBytes = 0, uploadNs = 0, decodes = 0, decodeNs = 0;
        uint64_t prefetchSkipped = 0;   // load-time prefetches refused: the budget was full
        uint64_t reuses = 0;            // uploads into a kept texture (no allocation)
        size_t pooledBytes = 0;         // kept textures now (gauge)
        tak::terrain::Compositor::Stats cpu;   // decoded images: bytes, pins, peak, evictions
    };
    SectionStats sectionStats() {
        SectionStats s = sectionStats_;
        s.decodes = sectionDecodes_.load(std::memory_order_relaxed);
        s.decodeNs = sectionDecodeNs_.load(std::memory_order_relaxed);
        s.prefetchSkipped = prefetchSkipped_.load(std::memory_order_relaxed);
        s.pooledBytes = freeSectionBytes_;
        s.cpu = comp_.stats();
        return s;
    }
    // CPU budget for decoded sections (workstream J): above kSectionCpuHigh the
    // compositor evicts least recently used, unpinned sections down to kSectionCpuLow.
    // 97.7% of the shipped maps decode to less (median 21 MiB, Ulasem Arena 64 MiB,
    // the largest 402 MiB), so on them nothing is ever evicted and behaviour is the
    // pre-J one: every referenced section decoded at load and kept.
    static constexpr size_t kSectionCpuHigh = size_t(128) << 20;
    static constexpr size_t kSectionCpuLow = size_t(112) << 20;
    // Sections used within this window are the camera's working set and are never
    // evicted (no decode/evict cycles while it keeps revisiting more than the
    // budget); the budget applies once they have been cold this long.
    static constexpr uint64_t kSectionMinIdleMs = 10000;
    // Every queued section JPG decoded (or skipped) and taken in. Tests only.
    bool sectionsDecoded() {
        std::lock_guard<std::mutex> lk(secMu_);
        return decodeQueue_.empty() && !secBusy_ && decoded_.empty();
    }

private:
    static constexpr int kBlock = 32;   // one map cell = a 32px tile

    // Load a real map from the VFS, OR -- when mapPath is a "~gen1~" random-map id --
    // build it procedurally in memory (client & server share the deterministic gen).
    static tak::tnt::Map genOrLoad(const tak::hpi::Vfs& vfs, const std::string& mapPath);

    // Collect the map's unique section keys and queue them for decode (a speculative
    // prefetch: skipped once the decoded sections reach the CPU budget's low mark).
    void queueAllSections();
    // Main thread: note the sections the worker found absent.
    void uploadReadySections();
    // Main thread: upload the wanted_ sections (a per-frame limit), first evicting idle
    // ones over the idle budget or the GPU cap. Returns whether any uploaded.
    bool uploadWantedSections();
    // Main thread: release sections idle past kSectionIdleMs, and any not drawn this
    // frame while over the idle budget or the GPU cap.
    void trimSections();
    // Worker thread: JPEG-decode queued sections into the Compositor cache.
    void sectionWorkerLoop();

    SDL_Renderer* ren_;
    tak::tnt::Map map_;
    tak::terrain::Compositor comp_;

    // Fallback terrain: where TerrainChunks has no image yet (still baking, just panned
    // in) or is switched off, visible 32px tiles draw as batched quads sampling their
    // section JPG, with a half-texel UV inset so bilinear filtering never bleeds across
    // tiles. Sections used to be uploaded at load and kept all session, duplicating the
    // chunks (64 MiB on Ulasem Arena, 238 MiB on Middle Earth V1). Now a section uploads
    // the frame the fallback first needs it (its JPG is already decoded in the
    // Compositor, so that is one texture upload), and is released once the chunks have
    // covered its tiles for kSectionIdleMs. kSectionBudget bounds what is kept IDLE (over
    // it, idle sections go at once); it never refuses a section the view needs this
    // frame -- a zoomed-out view of a big map with nothing baked can need every section
    // for a moment, and refusing showed the blurry underlay (measured on Middle Earth V1
    // in a fast sweep: 1.8% of frames at a hard 64 MiB, 0.05% with idle-only). Only the
    // GPU cap refuses; then the underlay shows, as it always did before an upload.
    struct Section { SDL_Texture* tex = nullptr; int w = 0, h = 0; uint64_t frame = 0, usedMs = 0; };
    std::map<uint32_t, Section> sections_;   // key -> resident texture, or tex=null = absent JPG
    std::vector<uint32_t> wanted_;           // sampled by the fallback this frame, not resident
    std::vector<uint32_t> batchKeys_;        // resident sections the current batch samples
    size_t sectionBytes_ = 0;
    uint64_t frame_ = 0;
    bool pinNext_ = false, pinning_ = false; // finishChunks(): the next draw has no upload limits
    static constexpr size_t kSectionBudget = size_t(64) << 20;
    static constexpr uint64_t kSectionIdleMs = 3000;
    uint64_t clockMs_ = 0;
    bool clockSet_ = false;
    uint64_t nowMs() const;
    static constexpr int kSectionUploadsPerFrame = 32;   // 32 MiB: a 4K jump in one frame
    // pool: keep the texture for the next section of its size (while the idle budget
    // has room), rather than destroying it; GPU pressure passes false.
    void evictSection(std::map<uint32_t, Section>::iterator, bool pool = true);
    // Released section textures kept for reuse, by size: a section the fallback needs
    // again refills one instead of allocating. They count as idle in kSectionBudget and
    // go first under GPU pressure.
    std::map<std::pair<int, int>, std::vector<SDL_Texture*>> freeSections_;
    size_t freeSectionBytes_ = 0;
    bool dropFreeSection();   // destroy one; false when none
    void dropFreeSections();
    // The resident section not drawn this frame that was used longest ago (end() = none).
    std::map<uint32_t, Section>::iterator oldestIdleSection();

    // Async section decode: the worker JPEG-decodes referenced sections into the
    // Compositor cache off-thread (the chunk worker reads the same cache); the main
    // thread uploads on demand what the cache holds (cachedSection never decodes) and
    // asks the worker, ahead of the prefetch, for what it does not hold (any more).
    struct DecodeRequest { uint32_t key; bool demand; };   // demand: the fallback needs it now
    std::thread secWorker_;
    std::mutex secMu_;
    std::condition_variable secCv_;
    std::deque<DecodeRequest> decodeQueue_;   // awaiting decode; demand requests in front
    std::vector<std::pair<uint32_t, bool>> decoded_;   // (key, found) done, awaiting the main thread
    std::set<uint32_t> secPending_;      // queued or in flight (dedup); decoded keys leave it
    bool secStop_ = false, secBusy_ = false;
    SectionStats sectionStats_;                       // main thread
    std::atomic<uint64_t> sectionDecodes_{0}, sectionDecodeNs_{0}, prefetchSkipped_{0};   // decode worker

    // Per-frame tile-quad batch, keyed by section texture; cached across frames
    // when the view is static (idle spectating rebuilds nothing).
    std::map<SDL_Texture*, std::vector<SDL_Vertex>> tileBatch_;
    // Linear sampling only: half-texel fades across boundaries between unrelated
    // crops, drawn over every tileBatch_ quad (see rebuildTileBatch).
    std::map<SDL_Texture*, std::vector<SDL_Vertex>> apronBatch_;
    tak::GeometrySubmit tileSubmit_;
    float builtOffX_ = 1e30f, builtOffY_ = 1e30f, builtZoom_ = -1;
    int builtW_ = -1, builtH_ = -1;
    bool tileBatchDirty_ = true;   // set when a section uploads / view changes
    size_t underlayTiles_ = 0;     // see underlayTiles()
#ifndef NDEBUG
    bool settleRestoreGate_ = false;   // debug TAK_TERRAIN_SETTLE lifted the calm gate this frame
#endif
    void rebuildTileBatch(int winW, int winH);
    tak::TerrainChunks chunks_{comp_};
    // One terrain upload budget per frame (workstream K): chunk images, and the fallback
    // sections that fill visible holes, priced by measured CPU submission cost.
    tak::terrainwork::UploadBudget uploadBudget_;
    uint64_t chunkRevision_=0;
    bool chunksEnabled_=true;

    bool bilinear_ = false;   // linear terrain sampling (see setBilinear)
    float offX_ = 0, offY_ = 0, zoom_ = 0.35f;
    float zoomSpeed_ = 1.0f;   // wheel-zoom sensitivity exponent (Options)
    SDL_Texture* underlay_ = nullptr;   // low-res overview drawn under tiles (not owned)
};
