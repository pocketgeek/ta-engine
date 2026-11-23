#pragma once
#include "gaf/animationtiming.h"
#include "tdf/tdf.h"
#include <algorithm>
#include <map>

namespace tak::gaf {
// Gameplay and data-hash callers share asset resolution. Pixel-only reskins
// remain cosmetic; the duration that controls feature replacement does not.
class FeatureBurnTiming {
    const hpi::Vfs& vfs_;
    std::map<std::string,uint32_t> effects_;
    std::map<std::string,std::map<std::string,uint32_t>> bodies_;
    uint32_t effect(const std::string& name) {
        if(name.empty())return 0;
        auto [it,added]=effects_.try_emplace(name,0);
        if(added)for(auto delay:animationTiming(vfs_,name))it->second+=std::max(1u,unsigned(delay));
        return it->second;
    }
    // Ticks one sequence of the feature's own `filename` GAF plays for (the
    // loader 493700 resolves every seqname* key there), each frame held for
    // max(1, delay) by the clock 5373d0; 0 when it is absent or empty.
    uint32_t body(const tdf::Node& definition,const char* key) {
        const auto file=hpi::MountSet::key(definition.valueOr("filename",""));
        const auto sequence=hpi::MountSet::key(definition.valueOr(key,""));
        if(file.empty() || sequence.empty())return 0;
        auto [bank,added]=bodies_.try_emplace(file);
        if(added)try {
            const auto path="anims/"+file+".gaf";
            for(const auto& animation:load(vfs_.read(path),Palette{},-1,path)) {
                uint32_t ticks=0;
                for(const auto& frame:animation.frames)ticks+=std::max(1u,unsigned(frame.retailDelayTicks));
                bank->second[hpi::MountSet::key(animation.name)]=ticks;
            }
        } catch(const std::exception&) {}
        const auto found=bank->second.find(sequence);
        return found==bank->second.end() ? 0 : found->second;
    }
public:
    explicit FeatureBurnTiming(const hpi::Vfs& vfs):vfs_(vfs) {}
    uint32_t duration(const tdf::Node& definition) {
        const auto ticks=body(definition,"seqnameburn");
        if(!ticks)return 0;
        const auto front=effect(hpi::MountSet::key(definition.valueOr("seqnamefrontflame","")));
        const auto back=effect(hpi::MountSet::key(definition.valueOr("seqnamebackflame","")));
        return front || back ? std::max(front,back) : ticks;
    }
    // A feature destroyed by damage plays seqnamedie (494ce0 mode 0, loaded
    // nonlooping at 493eff) and becomes its featuredead stage when the clock
    // runs out (495ef7). 0: no such sequence, so it is replaced at once.
    uint32_t dieDuration(const tdf::Node& definition) { return body(definition,"seqnamedie"); }
};
} // namespace tak::gaf
