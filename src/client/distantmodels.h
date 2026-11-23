#pragma once
#include <SDL.h>
#include <array>
#include <span>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace tak {
// Render-only impostors for tiny on-screen models. Cache memory and repaint work
// are bounded; unsupported targets/blending and overflow retain full geometry.
class DistantModels {
public:
    struct Item {
        // Stable cache identity. GameView passes the unit's geometry-entry handle
        // (index + generation, client/geometryowners.h), never the bare unit id:
        // ids restart after a world reset and a held image must never pass to
        // another unit.
        uint64_t id=~uint64_t(0);
        uint64_t revision=0;
        uint32_t textureTick=0; // preserve animated-image refresh cadence independently of geometry reuse
        float x=0,y=0,zoom=1;
        bool eligible=false;
        // Workstream G: the body was not generated this frame because holds()
        // predicted its image is shown without a repaint. `source`/`runs` are the
        // last generated body (same vertex count, older content). prepare() calls
        // its materialize callback before it reads them for a bake.
        bool bodyDeferred=false;
        std::span<const SDL_Vertex> source;
        std::span<const std::pair<SDL_Texture*,int>> runs;
        SDL_Texture* texture=nullptr;
        std::array<SDL_Vertex,6> quad{};
    };
    // Generates the deferred body of items[index] and refreshes its source, runs
    // and revision (render thread). Only reached when a prediction was wrong.
    using Materialize=void(*)(void* context,size_t index);
    // Draws one run of a bake in place of SDL_RenderGeometry (and counts it);
    // GameView routes indexed palette runs (workstream M) through it.
    using DrawRun=int(*)(void* context,SDL_Renderer*,SDL_Texture*,const SDL_Vertex*,int count);
    void setRunDrawer(DrawRun draw,void* context) { drawRun_=draw; drawRunContext_=context; }
    // The image pages' blend mode: premultiplied colour (ONE, ONE_MINUS_SRC_ALPHA, ADD
    // for colour and alpha).
    static SDL_BlendMode pageBlendMode();
    // The rules, shared with GameView's lookup before body generation.
    static constexpr float kMaxZoom=0.6f;          // eligible at this zoom or below
    static constexpr size_t kMinVertices=96;       // smaller bodies stay live
    static constexpr uint64_t kRefreshMs=67;       // changed content held this long
    static constexpr size_t kBakeBudget=128;       // bakes per prepare()
    static bool candidate(const Item& item) {
        return item.eligible && item.zoom<=kMaxZoom && item.source.size()>=kMinVertices;
    }
    void prepare(SDL_Renderer*,std::span<Item>,uint64_t now,Materialize materialize=nullptr,void* context=nullptr);
    // Workstream G: will prepare() show this identity's existing image without a
    // repaint, whatever its content is now? True when the image is valid, was
    // baked at this zoom and output scale and less than kRefreshMs ago -- exactly
    // the case where prepare() reads neither the revision nor the vertices. The
    // caller still applies candidate() (eligibility, zoom, vertex count). Read-only
    // (the geometry workers call it concurrently while nothing writes the cache).
    bool holds(uint64_t id,float zoom,float sx,float sy,uint64_t now) const;
    void clear(); // renderer must still be alive
    size_t used=0, refreshed=0;
    size_t bakeVertices=0,bakeDraws=0,targetSwitches=0;
    // Telemetry, per prepare(): lookups that reused an image (hits; held = inputs
    // changed but inside the refresh interval), bake causes, and refusals that
    // left the body on live geometry.
    size_t hits=0,held=0,missNew=0,missScale=0,missContent=0;
    size_t rejectBudget=0,rejectShape=0,rejectPages=0;
    // Workstream G, per prepare(): bounds taken from the retained metadata instead
    // of a vertex scan, deferred bodies prepare() had to generate after all, pages
    // released after their last image expired, images moved to another page.
    size_t boundsReused=0,lateBodies=0,pageReleases=0,migrated=0;
    size_t bytes() const {return livePages()*size_t(pageSize)*pageSize*4;}
    size_t livePages() const;
    size_t liveTiles() const {return entries_.size();}
    size_t metadataBytes() const; // CPU: entries, bounds, page maps, free list, scratch
    // Tests/debug: the page bookkeeping is consistent (every image on a live page,
    // occupancy bits and counts agree, and for every tile position the free list
    // plus the images there equal the pages the pre-G code would hold). Returns
    // nullptr or what is wrong.
    const char* validate() const;
    // Tests only: also run on SDL's software renderer (the GPU rules otherwise
    // keep it on live geometry).
    bool allowSoftware=false;
    // Debug/tests: an image page is released, or images migrate off the highest
    // page, only during the periodic sweep (every 60 prepares). Forcing the sweep
    // every prepare exercises those paths in short scenes.
    bool sweepEveryPrepare=false;

    static constexpr int tileSize=64,pageSize=2048,tilesPerPage=1024,maxPages=4;
    // Images on the highest page are moved to lower pages when at most this many
    // remain and each can keep its tile position (bounded per sweep).
    static constexpr int kMigrateMax=256;
private:
    DrawRun drawRun_=nullptr;
    void* drawRunContext_=nullptr;
    struct Entry {
        int slot=-1;
        uint64_t seen=0,painted=0,revision=0;
        uint32_t textureTick=0;
        uintptr_t source=0;
        float x=0,y=0,w=0,h=0,zoom=1,sx=1,sy=1;
        int pixelsW=0,pixelsH=0;
        bool valid=false;
    };
    // Bounds and opacity of a body, kept for the content they were measured on
    // (identity + revision + source address + vertex count): a body refused for
    // its shape or for page room is not rescanned every frame to find that out
    // again. Kept apart from entries_ so the image entries' history -- and with it
    // the order expired tiles are reused in -- is exactly the pre-G one.
    struct Bounds {
        uint64_t revision=0,seen=0;
        uintptr_t source=0;
        size_t count=0;
        float minX=0,minY=0,maxX=0,maxY=0;
        bool opaque=false;
    };
    struct Bake {size_t item;Entry* entry;float minX,minY;int pw,ph;};
    std::unordered_map<uint64_t,Entry> entries_;
    std::unordered_map<uint64_t,Bounds> bounds_;
    // Page slots; nullptr = released (the slot index is reused by the next page).
    std::vector<SDL_Texture*> pages_;
    std::vector<int> pageLive_;                      // images per page
    std::vector<std::array<uint64_t,16>> pageUsed_;  // tile occupancy bit per page
    // Free tiles, last freed first, exactly as before G. Only an element's tile
    // POSITION is used: a tile's pixels (bake) and texture coordinates (composite)
    // depend on its position in the page, never on the page, so a new image takes
    // that position on the lowest live page where it is free.
    std::vector<int> free_;
    std::vector<SDL_Vertex> scratch_;
    std::vector<Bake> bakes_;
    std::vector<Entry*> shown_;                      // per item: entry to composite, or null
    uint64_t frame_=0;
    int formerPages_=0;   // pages the pre-G code would hold (it released none until all expired)
    bool unsupported_=false;
    bool tileUsed(int page,int local) const {return (pageUsed_[size_t(page)][size_t(local>>6)]>>(local&63))&1u;}
    void markTile(int page,int local,bool used);
    int createPage(SDL_Renderer*,int at);
    void releasePage(int page);
    int takeSlot(SDL_Renderer*);
    void sweep(SDL_Renderer*);
};
}
