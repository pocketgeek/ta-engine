#include "cartographer/features.h"

#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <filesystem>

namespace cart {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
} // namespace

void FeatureLibrary::scan(const ta::hpi::Vfs& vfs, const std::string& world) {
    refs_.clear();
    spriteCache_.clear();
    const std::string dirs[] = {"features/" + lower(world), "features/all worlds"};
    for (const std::string& dir : dirs) {
        for (const std::string& path : vfs.list(dir)) {
            if (std::filesystem::path(lower(path)).extension() != ".tdf") continue;
            ta::tdf::Node root;
            try {
                auto d = vfs.read(path);
                root = ta::tdf::parseText(std::string(d.begin(), d.end()), path);
            } catch (const std::exception&) { continue; }
            for (const std::string& sec : root.childOrder) {
                const ta::tdf::Node* n = root.child(sec);
                if (!n) continue;
                FeatureRef r;
                r.name = sec;   // childOrder is already lowercased
                r.category = lower(n->valueOr("category", ""));
                r.filename = lower(n->valueOr("filename", ""));
                r.seqname = lower(n->valueOr("seqname", ""));
                r.world = lower(n->valueOr("world", world));
                // Only placeable art features (skip corpses / defs with no art).
                if (!r.filename.empty() && !r.seqname.empty())
                    refs_.push_back(std::move(r));
            }
        }
    }
    std::sort(refs_.begin(), refs_.end(), [](const FeatureRef& a, const FeatureRef& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
}

const FeatureRef* FeatureLibrary::byName(const std::string& name) const {
    std::string lo = lower(name);
    for (const auto& r : refs_) if (r.name == lo) return &r;
    return nullptr;
}

const FeatSprite* FeatureLibrary::sprite(const ta::hpi::Vfs& vfs, const FeatureRef& r) {
    auto it = spriteCache_.find(r.name);
    if (it != spriteCache_.end()) return &it->second;
    FeatSprite fs;
    // Palette: <world>_features.pcx, else <world>.pcx, else aramon_features.
    const std::string palCands[] = {r.world + "_features.pcx", r.world + ".pcx",
                                     "aramon_features.pcx"};
    ta::gaf::Palette pal{};
    bool havePal = false;
    for (const std::string& c : palCands) {
        try {
            pal = ta::gaf::Palette::fromBytes(vfs.read("palettes/" + c), c);
            havePal = true; break;
        } catch (const std::exception&) {}
    }
    if (havePal) {
        try {
            std::string gp = "anims/" + r.filename + ".gaf";
            for (auto& sq : ta::gaf::load(vfs.read(gp), pal, -1, gp)) {
                if (!lower(sq.name).empty() && lower(sq.name) == r.seqname &&
                    !sq.frames.empty() && sq.frames[0].width > 0) {
                    const auto& fr = sq.frames[0];
                    fs.w = fr.width; fs.h = fr.height;
                    fs.xoff = fr.xoff; fs.yoff = fr.yoff;
                    fs.rgba = fr.rgba;
                    break;
                }
            }
        } catch (const std::exception&) {}
    }
    return &spriteCache_.emplace(r.name, std::move(fs)).first->second;
}

} // namespace cart
