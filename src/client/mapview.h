#pragma once

// Terrain map view: pans/zooms a TNT map and draws it as batched tile quads out
// of one packed atlas. Extracted from client/main.cpp; kept at global scope so
// its unqualified use sites there are unchanged.
//
// Lifetime: the Vfs passed to the ctor/reload MUST outlive the MapView — the
// terrain::Compositor borrows it by reference. (GameView declares its vfs_
// member before its mapView_ member for exactly this reason.)
// Owns a GPU texture, so it is non-copyable; declared non-movable to match.

#include <SDL.h>

#include "terrain/terrain.h"   // ta::terrain::Compositor (by-value member)
#include "tnt/tnt.h"           // ta::tnt::Map (by-value member)

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ta::hpi { class Vfs; }

class MapView {
public:
    MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, const std::string& mapPath);
    // Construct directly from an in-memory map (e.g. the editor's fresh/blank map).
    MapView(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, ta::tnt::Map map);
    ~MapView();
    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    // Swap in a different map (rebuilding the tile atlas). Used at game start so
    // the render terrain matches the map the sim actually loaded.
    void reload(const ta::hpi::Vfs& vfs, const std::string& mapPath);

    void input(const SDL_Event& e);

    // The smallest zoom at which the map still fills the window in one dimension
    // (zooming out past this would run the view off the map edges).
    float minZoom(int winW, int winH) const;
    // Apply just the min-zoom floor (no offset change).
    void clampZoom(int winW, int winH);
    void clampOffset(int winW, int winH);

    // Kept for its call sites: terrain upload is now a single atlas built at map
    // load, so this only keeps the view on the map.
    void ensureChunks(int winW, int winH);

    // Screenshot paths only. The atlas is resident from load, so there is nothing
    // left to wait for; retained so those call sites need not change.
    void finishChunks() {}

    // A low-res whole-map overview (one texel per 32px block), drawn UNDER the
    // tile grid so the map still reads if the atlas could not be uploaded. The
    // owner sets this (GameView's minimap texture); null = none.
    void setUnderlay(SDL_Texture* t) { underlay_ = t; }

    void draw(int winW, int winH);

    // Bilinear terrain scaling (retail's video option).
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
    // Mutable terrain access: edit map().tiles/tileGfx/heights/features, then
    // call tilesEdited() so the atlas is rebuilt and the tile-quad batch
    // refreshes next frame.
    ta::tnt::Map& editMap() { return map_; }
    void tilesEdited() { buildAtlas(); tileBatchDirty_ = true; }

private:
    static constexpr int kBlock = 32;   // one map cell = a 32px tile

    // Load a real map from the VFS, OR -- when mapPath is a "~gen1~" random-map id --
    // build it procedurally in memory (client & server share the deterministic gen).
    static ta::tnt::Map genOrLoad(const ta::hpi::Vfs& vfs, const std::string& mapPath);

    // Expand the map's whole tile library through the palette and upload it as
    // ONE atlas texture. Cheap enough to do synchronously at load.
    void buildAtlas();
    void destroyAtlas();

    SDL_Renderer* ren_;
    ta::tnt::Map map_;
    ta::terrain::Compositor comp_;

    // Terrain rendering: a TA map carries its OWN tile library (up to a few
    // thousand 32x32 8-bit tiles), so rather than one texture per tile -- which
    // would be thousands of draw calls -- the whole library is expanded through
    // the palette and packed into a SINGLE atlas texture at load. Visible tiles
    // then draw as batched quads into that one texture, with a half-texel UV
    // inset so bilinear filtering never bleeds across neighbouring cells.
    //
    // (Kingdoms referenced shared JPG sections instead, a handful per map, so it
    // decoded them on a worker and uploaded one texture each. There is no decode
    // to hide here -- expanding indices through a 256-entry palette is a memcpy's
    // worth of work -- so the worker thread is gone with it.)
    SDL_Texture* atlas_ = nullptr;
    int atlasCols_ = 0, atlasW_ = 0, atlasH_ = 0;

    // Per-frame tile-quad batch, keyed by texture (one key: the atlas); cached
    // when the view is static (idle spectating rebuilds nothing).
    std::map<SDL_Texture*, std::vector<SDL_Vertex>> tileBatch_;
    float builtOffX_ = 1e30f, builtOffY_ = 1e30f, builtZoom_ = -1;
    int builtW_ = -1, builtH_ = -1;
    bool tileBatchDirty_ = true;   // set when the atlas rebuilds / view changes
    void rebuildTileBatch(int winW, int winH);

    bool bilinear_ = false;   // smooth terrain scaling (Options; see setBilinear)
    float offX_ = 0, offY_ = 0, zoom_ = 0.35f;
    float zoomSpeed_ = 1.0f;   // wheel-zoom sensitivity exponent (Options)
    SDL_Texture* underlay_ = nullptr;   // low-res overview drawn under tiles (not owned)
};
