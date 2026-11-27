#include "cartographer/generator.h"
#include "hpi/hpi.h"
#include "util/virtualpath.h"
#include <charconv>
#include <algorithm>
#include <cctype>
namespace cart {
std::string mapGeneratorRecipe(const ta::hpi::Vfs& vfs,const std::string& mapPath,const std::string& embedded) {
    if(!embedded.empty())return embedded;
    const auto path=ta::vpath::replaceExtension(mapPath,".recipe");
    if(!vfs.has(path))return {};
    const auto bytes=vfs.read(path);return {bytes.begin(),bytes.end()};
}
GeneratorFields generatorFields(const ta::mapgen::Params& p) {
    return {std::to_string(p.seed),std::to_string(p.players),std::to_string(p.reliefDensity)};
}
bool parseGeneratorFields(const GeneratorFields& f,ta::mapgen::Params& p,std::string& error) {
    auto next=p;
    auto number=[&](size_t i,uint64_t min,uint64_t max,uint64_t& value) {
        const auto result=std::from_chars(f[i].data(),f[i].data()+f[i].size(),value);
        return result.ec==std::errc{} && result.ptr==f[i].data()+f[i].size() && value>=min && value<=max;
    };
    uint64_t value=0;
    if(!number(0,0,UINT64_MAX,value)) {error="Seed must be an unsigned 64-bit whole number.";return false;}next.seed=value;
    if(!number(1,2,ta::mapgen::kMaxPlayers,value)) {error="Choose 2 to "+std::to_string(ta::mapgen::kMaxPlayers)+" players.";return false;}next.players=uint8_t(value);
    if(!number(2,0,255,value)) {error="Hills must be a whole number from 0 to 255.";return false;}next.reliefDensity=uint8_t(value);
    p=next;error.clear();return true;
}
bool restoreGeneratorRecipe(const std::string& recipe,ta::mapgen::Params& params,std::string& error) {
    auto encoded=recipe;
    if(encoded.empty()) {error="This map has no saved generator recipe. Use New Map > Random to create one.";return false;}
    // decodeMapId intentionally supplies defaults on malformed input. The editor
    // must reject that fallback: otherwise Regenerate could replace a map with an
    // unrelated seed while appearing to restore its original settings. A recipe of
    // another generator version does not encode back to itself either.
    std::transform(encoded.begin(),encoded.end(),encoded.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto restored=ta::mapgen::decodeMapId(encoded);
    if(!ta::mapgen::isGeneratedMapId(encoded) || ta::mapgen::encodeMapId(restored)!=encoded) {
        error="The saved generator recipe is malformed or cannot be restored exactly.";return false;
    }
    params=restored;error.clear();return true;
}
}
