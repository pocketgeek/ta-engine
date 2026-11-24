#pragma once
#include "terrain/terrain.h"
#include <SDL.h>
#include <future>
#include <map>
#include <memory>

namespace cart {
// A unit's build picture for Thumbnails: TA's unitpics/<id>.pcx, else a
// Kingdoms-style anims/buildpic/<id>.jpg (a mod's).
std::string unitPortraitPath(const ta::hpi::Vfs& assets,const std::string& unitId);
// SDL resources stay on the UI thread. Only decoding/composition runs in a
// worker. A path names what to show: a terrain section (.sct, or a Kingdoms
// .tnt prefab) rendered from its tiles, a TA map (.tnt) by its own radar
// picture, a TA build picture (unitpics/<id>.pcx) or a .jpg portrait. Renders
// are shrunk to kMaxSide. reset() must precede replacing the borrowed VFS.
class Thumbnails {
public:
    static constexpr int kMaxSide=512;
    Thumbnails(SDL_Renderer* renderer,const ta::hpi::Vfs& assets,size_t limit=128);
    ~Thumbnails();
    SDL_Texture* get(const std::string& path);
    void reset();
    bool loading() const {return job_.valid();}
    // True once `path` has a decode verdict (texture or failure); false while queued/in flight.
    bool resolved(const std::string& path) const {return cache_.count(path)!=0;}
    size_t entries() const {return cache_.size();}
private:
    struct Entry {SDL_Texture* texture=nullptr;size_t bytes=0;uint64_t used=0;};
    struct Result {std::string path;ta::jpeg::Image image;};
    static ta::jpeg::Image shrink(ta::jpeg::Image image);
    SDL_Renderer* renderer_;
    const ta::hpi::Vfs& assets_;
    std::unique_ptr<ta::terrain::Compositor> compositor_;
    std::map<std::string,Entry> cache_;
    std::future<Result> job_;
    size_t limit_,bytes_=0,decoded_=0;
    uint64_t clock_=0;
};
}
