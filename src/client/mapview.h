#pragma once

// Terrain map view: pans/zooms a TNT map and composites its tile chunks on a
// background worker thread (see chunkWorkerLoop in mapview.cpp). Extracted from
// client/main.cpp; kept at global scope so its unqualified use sites there are
// unchanged.
//
// Lifetime: the Vfs passed to the ctor/reload MUST outlive the MapView — the
// terrain::Compositor borrows it by reference. (GameView declares its vfs_
// member before its mapView_ member for exactly this reason.)
// Owns a worker thread + mutex, so it is implicitly non-copyable/non-movable.

#include <SDL.h>

#include "terrain/terrain.h"   // ta::terrain::Compositor (by-value member)
#include "tnt/tnt.h"           // ta::tnt::Map (by-value member)

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

namespace ta::hpi { class Vfs; }

class MapView {
public:
    MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, const std::string& mapPath);
    // Construct directly from an in-memory map (e.g. the editor's fresh/blank map).
    MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, ta::tnt::Map map);
    ~MapView();

    // Swap in a different map (discarding cached chunk textures). The compositor's
    // decoded-tile cache is content-addressed by tile key, so it stays valid. Used at
    // game start so the render terrain matches the map the sim actually loaded.
    void reload(const ta::hpi::Vfs& vfs, const std::string& mapPath);

    void input(const SDL_Event& e);

    // The smallest zoom at which the map still fills the window in one dimension
    // (zooming out past this would run the view off the map edges).
    float minZoom(int winW, int winH) const;
    // Apply just the min-zoom floor (no offset change).
    void clampZoom(int winW, int winH);
    void clampOffset(int winW, int winH);

    // Create any terrain chunk textures that will be visible this frame. Called
    // BEFORE the render pass so texture creation never interleaves with draw calls.
    void ensureChunks(int winW, int winH);

    // Block until every queued chunk is composited and uploaded. Screenshot paths
    // only -- normal play never waits.
    void finishChunks();

    // A low-res whole-map overview (one texel per 32px block), drawn UNDER the chunk
    // grid so a not-yet-composited chunk shows blurry terrain instead of a black
    // rectangle. The owner sets this (GameView's minimap texture); null = none.
    void setUnderlay(SDL_Texture* t) { underlay_ = t; }

    void draw(int winW, int winH);

    // Bilinear terrain scaling (retail's video option). Applies to already-built
    // chunk textures immediately and to every chunk composited afterwards.
    void setBilinear(bool b);

    float offX() const { return offX_; }
    float offY() const { return offY_; }
    float zoom() const { return zoom_; }
    ta::terrain::Compositor& compositor() { return comp_; }
    void setZoom(float z) { zoom_ = z; }
    void setOffset(float x, float y) { offX_ = x; offY_ = y; }
    void setZoomSpeed(float m);
    const ta::tnt::Map& map() const { return map_; }

    // --- Editing (Cartographer) ------------------------------------------------
    // Mutable terrain access: edit map().tileKeys/tileCols/tileRows/heights/
    // features, then call tilesEdited() so any newly-referenced section textures
    // decode+upload and the tile-quad batch rebuilds next frame.
    ta::tnt::Map& editMap() { return map_; }
    void tilesEdited() { queueAllSections(); tileBatchDirty_ = true; }

private:
    static constexpr int kBlock = 32;   // one map cell = a 32px tile

    // Load a real map from the VFS, OR -- when mapPath is a "~gen1~" random-map id --
    // build it procedurally in memory (client & server share the deterministic gen).
    static ta::tnt::Map genOrLoad(const ta::hpi::Vfs& vfs, const std::string& mapPath);

    // Collect the map's unique section keys and queue them for decode.
    void queueAllSections();
    // Main thread: turn decoded section JPGs into GPU textures (budgeted/frame).
    void uploadReadySections();
    // Worker thread: JPEG-decode queued sections into the Compositor cache.
    void sectionWorkerLoop();

    SDL_Renderer* ren_;
    ta::tnt::Map map_;
    ta::terrain::Compositor comp_;

    // Terrain rendering: each referenced section JPG is uploaded ONCE as a GPU
    // texture (~7 MiB for a whole map, resolution-independent -- the old 512px
    // composite chunks cost ~1 MiB PER visible screen-chunk, hundreds of MiB at
    // 4K). Visible 32px tiles draw as batched quads sampling their section, with
    // a half-texel UV inset so bilinear filtering never bleeds across tiles.
    struct Section { SDL_Texture* tex = nullptr; int w = 0, h = 0; };
    std::map<uint32_t, Section> sections_;   // key -> uploaded texture (main thread)

    // Async section decode: the worker JPEG-decodes referenced sections into the
    // Compositor cache off-thread; the main thread uploads them (budgeted). Until
    // a section is up the underlay shows through, exactly like the old chunks.
    std::thread secWorker_;
    std::mutex secMu_;
    std::condition_variable secCv_;
    std::deque<uint32_t> decodeQueue_;   // keys awaiting decode
    std::vector<uint32_t> decoded_;      // keys decoded, awaiting GPU upload
    std::set<uint32_t> secPending_;      // queued / in flight / uploaded (dedup)
    bool secStop_ = false, secBusy_ = false;

    // Per-frame tile-quad batch, keyed by section texture; cached across frames
    // when the view is static (idle spectating rebuilds nothing).
    std::map<SDL_Texture*, std::vector<SDL_Vertex>> tileBatch_;
    float builtOffX_ = 1e30f, builtOffY_ = 1e30f, builtZoom_ = -1;
    int builtW_ = -1, builtH_ = -1;
    bool tileBatchDirty_ = true;   // set when a section uploads / view changes
    void rebuildTileBatch(int winW, int winH);

    bool bilinear_ = false;   // smooth terrain scaling (Options; see setBilinear)
    float offX_ = 0, offY_ = 0, zoom_ = 0.35f;
    float zoomSpeed_ = 1.0f;   // wheel-zoom sensitivity exponent (Options)
    SDL_Texture* underlay_ = nullptr;   // low-res overview drawn under tiles (not owned)
};
