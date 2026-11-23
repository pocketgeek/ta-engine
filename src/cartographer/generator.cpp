#include "cartographer/generator.h"
#include "hpi/hpi.h"
#include "util/virtualpath.h"
#include <charconv>
#include <algorithm>
#include <cctype>
namespace cart {
std::string mapGeneratorRecipe(const tak::hpi::Vfs& vfs,const std::string& mapPath,const std::string& embedded) {
    if(!embedded.empty())return embedded;
    const auto path=tak::vpath::replaceExtension(mapPath,".recipe");
    if(!vfs.has(path))return {};
    const auto bytes=vfs.read(path);return {bytes.begin(),bytes.end()};
}
GeneratorFields generatorFields(const tak::mapgen::Params& p) {
    return {std::to_string(p.seed),std::to_string(p.players),tak::mapgen::layoutName(p.layout),
        std::to_string(p.treeDensity),std::to_string(p.rockDensity),std::to_string(p.manaDensity),
        std::to_string(p.waterDensity),std::to_string(p.reliefDensity),tak::mapgen::scatterName(p.manaScatter),
        std::to_string(p.plateauDensity),std::to_string(p.mountainDensity),std::to_string(p.townDensity)};
}
bool parseGeneratorFields(const GeneratorFields& f,tak::mapgen::Params& p,std::string& error) {
    auto next=p;
    auto number=[&](size_t i,uint64_t min,uint64_t max,uint64_t& value) {
        const auto result=std::from_chars(f[i].data(),f[i].data()+f[i].size(),value);
        return result.ec==std::errc{} && result.ptr==f[i].data()+f[i].size() && value>=min && value<=max;
    };
    uint64_t value=0;
    if(!number(0,0,UINT64_MAX,value)) {error="Seed must be an unsigned 64-bit whole number.";return false;}next.seed=value;
    if(!number(1,2,8,value)) {error="Choose 2 to 8 players.";return false;}next.players=uint8_t(value);
    bool layout=false;
    for(uint8_t i=0;i<tak::mapgen::kLayouts;++i)if(f[2]==tak::mapgen::layoutName(i)) {next.layout=i;layout=true;}
    if(!layout) {error="Choose a layout.";return false;}
    uint8_t* density[]={&next.treeDensity,&next.rockDensity,&next.manaDensity,&next.waterDensity,&next.reliefDensity};
    for(size_t i=3;i<8;++i) {
        if(!number(i,0,255,value)) {error="Densities must be whole numbers from 0 to 255.";return false;}
        *density[i-3]=uint8_t(value);
    }
    bool scatter=false;
    for(uint8_t i=0;i<tak::mapgen::kScatterLevels;++i)if(f[8]==tak::mapgen::scatterName(i)) {next.manaScatter=i;scatter=true;}
    if(!scatter) {error="Choose a scattered mana level.";return false;}
    uint8_t* kits[]={&next.plateauDensity,&next.mountainDensity,&next.townDensity};
    for(size_t i=9;i<12;++i) {
        if(!number(i,0,255,value)) {error="Plateaus, mountains and towns must be whole numbers from 0 to 255.";return false;}
        *kits[i-9]=uint8_t(value);
    }
    p=next;error.clear();return true;
}
bool restoreGeneratorRecipe(const std::string& recipe,tak::mapgen::Params& params,std::string& error) {
    auto encoded=recipe;
    if(encoded.empty()) {error="This map has no saved generator recipe. Use New Map > Random to create one.";return false;}
    // decodeMapId intentionally supplies defaults on malformed input. The editor
    // must reject that fallback: otherwise Regenerate could replace a map with an
    // unrelated seed while appearing to restore its original settings. A recipe of
    // another generator version does not encode back to itself either.
    std::transform(encoded.begin(),encoded.end(),encoded.begin(),[](unsigned char c){return char(std::tolower(c));});
    const auto restored=tak::mapgen::decodeMapId(encoded);
    if(!tak::mapgen::isGeneratedMapId(encoded) || tak::mapgen::encodeMapId(restored)!=encoded) {
        error="The saved generator recipe is malformed or cannot be restored exactly.";return false;
    }
    params=restored;error.clear();return true;
}
}
