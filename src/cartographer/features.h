#pragma once

// Cartographer's Features palette: the placeable map objects (trees, rocks,
// mana, ruins...) defined by features/<world>/*.tdf + features/all worlds/*.tdf.
// Placing one writes its NAME into the .tnt feature plane (interned into the
// feature-name table); the game resolves the same name at load.

#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

namespace cart {

struct FeatureRef {
    std::string name;       // TDF section name (what goes in the feature table)
    std::string category;   // e.g. trees / rocks / mana
    std::string filename;   // GAF stem (anims/<filename>.gaf)
    std::string seqname;    // sequence within the GAF
    std::string world;      // for the palette
    int footX = 1, footZ = 1;   // footprintx/z cells from the anchor (symmetry)
};

// One decoded feature sprite (GAF frame 0), RGBA + its anchor.
struct FeatSprite {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, xoff = 0, yoff = 0;
};

class FeatureLibrary {
public:
    // Scan features/<world> + features/all worlds for placeable feature defs.
    void scan(const tak::hpi::Vfs& vfs, const std::string& world);
    const std::vector<FeatureRef>& list() const { return refs_; }
    const FeatureRef* byName(const std::string& name) const;   // nullptr if absent
    // Decode (and cache) a feature's sprite; empty (.w==0) if the art won't load.
    const FeatSprite* sprite(const tak::hpi::Vfs& vfs, const FeatureRef& r);
    // Nonblocking editor path: nullptr while art is queued/decoding. A worker
    // owns only a copied feature reference and borrows the immutable VFS.
    const FeatSprite* requestSprite(const tak::hpi::Vfs& vfs,const FeatureRef& r);
    // Must precede replacing the borrowed VFS. scan() also drains old work.
    void quiesce();
    bool loading() const {return job_.valid();}
    // Bumped by scan(): indices into list() from an earlier scan are void.
    unsigned generation() const { return generation_; }

private:
    unsigned generation_ = 0;
    std::vector<FeatureRef> refs_;
    std::map<std::string,size_t> names_;
    std::map<std::string, FeatSprite> spriteCache_;   // keyed by feature name
    struct Result {std::string name;FeatSprite sprite;};
    std::future<Result> job_; // destroyed/joined before the cache and references
    static FeatSprite decodeSprite(const tak::hpi::Vfs& vfs,const FeatureRef& r);
};

} // namespace cart
