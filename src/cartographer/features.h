#pragma once

// Cartographer's Features palette: the placeable map objects (trees, rocks,
// mana, ruins...) defined by features/<world>/*.tdf + features/all worlds/*.tdf.
// Placing one writes its NAME into the .tnt feature plane (interned into the
// feature-name table); the game resolves the same name at load.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ta::hpi { class Vfs; }

namespace cart {

struct FeatureRef {
    std::string name;       // TDF section name (what goes in the feature table)
    std::string category;   // e.g. trees / rocks / mana
    std::string filename;   // GAF stem (anims/<filename>.gaf)
    std::string seqname;    // sequence within the GAF
    std::string world;      // for the palette
};

// One decoded feature sprite (GAF frame 0), RGBA + its anchor.
struct FeatSprite {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, xoff = 0, yoff = 0;
};

class FeatureLibrary {
public:
    // Scan features/<world> + features/all worlds for placeable feature defs.
    void scan(const ta::hpi::Vfs& vfs, const std::string& world);
    const std::vector<FeatureRef>& list() const { return refs_; }
    const FeatureRef* byName(const std::string& name) const;   // nullptr if absent
    // Decode (and cache) a feature's sprite; empty (.w==0) if the art won't load.
    const FeatSprite* sprite(const ta::hpi::Vfs& vfs, const FeatureRef& r);

private:
    std::vector<FeatureRef> refs_;
    std::map<std::string, FeatSprite> spriteCache_;   // keyed by feature name
};

} // namespace cart
