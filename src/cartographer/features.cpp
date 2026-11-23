#include "cartographer/features.h"

#include "gaf/gaf.h"
#include "hpi/hpi.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <chrono>
#include "util/virtualpath.h"

namespace cart {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){return char(std::tolower(c));});
    return s;
}
} // namespace

void FeatureLibrary::scan(const tak::hpi::Vfs& vfs, const std::string& world) {
    quiesce();
    refs_.clear();names_.clear();
    spriteCache_.clear();++generation_;
    const std::string dirs[] = {"features/" + lower(world), "features/all worlds"};
    for (const std::string& dir : dirs) {
        for (const std::string& path : vfs.list(dir)) {
            if (tak::vpath::extension(lower(path)) != ".tdf") continue;
            tak::tdf::Node root;
            try {
                auto d = vfs.read(path);
                root = tak::tdf::parseText(std::string(d.begin(), d.end()), path);
            } catch (const std::exception&) { continue; }
            for (const std::string& sec : root.childOrder) {
                const tak::tdf::Node* n = root.child(sec);
                if (!n) continue;
                FeatureRef r;
                r.name = sec;   // childOrder is already lowercased
                r.category = lower(n->valueOr("category", ""));
                r.filename = lower(n->valueOr("filename", ""));
                r.seqname = lower(n->valueOr("seqname", ""));
                r.world = lower(n->valueOr("world", world));
                r.footX = std::max(1, int(n->numberOr("footprintx", 1)));
                r.footZ = std::max(1, int(n->numberOr("footprintz", 1)));
                // Only placeable art features (skip corpses / defs with no art).
                if (!r.filename.empty() && !r.seqname.empty())
                    refs_.push_back(std::move(r));
            }
        }
    }
    std::sort(refs_.begin(), refs_.end(), [](const FeatureRef& a, const FeatureRef& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    for(size_t i=0;i<refs_.size();++i)names_.emplace(refs_[i].name,i);
}

const FeatureRef* FeatureLibrary::byName(const std::string& name) const {
    const auto found=names_.find(lower(name));
    return found==names_.end()?nullptr:&refs_[found->second];
}

const FeatSprite* FeatureLibrary::sprite(const tak::hpi::Vfs& vfs, const FeatureRef& r) {
    auto it = spriteCache_.find(r.name);
    if (it != spriteCache_.end()) return &it->second;
    return &spriteCache_.emplace(r.name,decodeSprite(vfs,r)).first->second;
}
void FeatureLibrary::quiesce() {
    if(job_.valid()) {job_.wait();job_=std::future<Result>{};}
}
const FeatSprite* FeatureLibrary::requestSprite(const tak::hpi::Vfs& vfs,const FeatureRef& r) {
    if(job_.valid() && job_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=job_.get();spriteCache_.emplace(std::move(result.name),std::move(result.sprite));
    }
    if(auto found=spriteCache_.find(r.name);found!=spriteCache_.end())return &found->second;
    if(!job_.valid())job_=std::async(std::launch::async,[&vfs,r] {
        return Result{r.name,decodeSprite(vfs,r)};
    });
    return nullptr;
}
FeatSprite FeatureLibrary::decodeSprite(const tak::hpi::Vfs& vfs,const FeatureRef& r) {
    FeatSprite fs;
    // Palette: <world>_features.pcx, else <world>.pcx, else aramon_features.
    const std::string palCands[] = {r.world + "_features.pcx", r.world + ".pcx",
                                     "aramon_features.pcx"};
    tak::gaf::Palette pal{};
    bool havePal = false;
    for (const std::string& c : palCands) {
        try {
            pal = tak::gaf::Palette::fromBytes(vfs.read("palettes/" + c), c);
            havePal = true; break;
        } catch (const std::exception&) {}
    }
    if (havePal) {
        try {
            std::string gp = "anims/" + r.filename + ".gaf";
            for (auto& sq : tak::gaf::load(vfs.read(gp), pal, -1, gp)) {
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
    return fs;
}

} // namespace cart
