#pragma once
#include "sim/retailhweffect.h"
#include "tdf/tdf.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tak {
struct RetailLightningDefinition {
    RetailLightningEffect initial;
    std::array<uint16_t,256> palette{};
    int32_t anchorX=0,anchorY=0;
    bool translucent=false;
};

// 4f4b70 / 4f5180 / 4f4200: load a named effect whose sources are line lightning.
// Duplicate emitter and palette sections are significant and retain file order.
inline RetailLightningDefinition retailLightningDefinition(const tdf::Node& node) {
    auto integer=[](double value) {
        if(!std::isfinite(value) || value<std::numeric_limits<int32_t>::min() ||
           value>std::numeric_limits<int32_t>::max())throw std::runtime_error("effect integer out of range");
        return int32_t(value);
    };
    auto number=[&](const tdf::Node& n,const char* key,int fallback=0) {
        return integer(n.numberOr(key,fallback));
    };
    auto fixed8=[](int32_t value){return std::bit_cast<int32_t>(uint32_t(value)<<8);};
    auto color=[](const tdf::Node& n,const char* key) {
        const auto value=n.valueOr(key,"");const char* cursor=value.c_str();uint32_t result=0;
        for(unsigned channel=0;channel<3;++channel) {
            char* end=nullptr;
            const auto component=std::strtol(cursor,&end,10);cursor=end;
            result|=uint32_t(uint8_t(component))<<(channel*8);
        }
        return result;
    };
    RetailLightningDefinition result;auto& effect=result.initial;
    effect.width=number(node,"width");effect.height=number(node,"height");
    if(effect.width<3 || effect.height<4)throw std::runtime_error("effect dimensions too small");
    result.anchorX=number(node,"anchorx");result.anchorY=number(node,"anchory");
    effect.capacity=uint32_t(number(node,"maxparticles",1024));
    effect.intensity=fixed8(number(node,"startintensity",16));
    if((effect.intensity>>8)==0)throw std::runtime_error("effect intensity has zero palette divisor");
    effect.decay=integer(node.numberOr("fadespeed",1)*256.0);
    effect.rise=number(node,"rise")!=0;
    result.translucent=number(node,"translucent")!=0;
    effect.pixels.assign(size_t(effect.width)*size_t(effect.height),0);
    if(const auto* emitters=node.child("emitters"))for(const auto& [name,source]:emitters->orderedChildren()) {
        if(name!="line lightning")throw std::runtime_error("unsupported named effect source: "+std::string(name));
        RetailLightningSource entry;
        entry.position={fixed8(number(*source,"x")),fixed8(number(*source,"y"))};
        entry.end={fixed8(number(*source,"x1")),fixed8(number(*source,"y1"))};
        entry.rate=number(*source,"updaterate");entry.fade=number(*source,"fade")!=0;
        for(const auto* point:{&entry.position,&entry.end})
            if((*point)[0]<0 || ((*point)[0]>>8)>=effect.width || (*point)[1]<0 || ((*point)[1]>>8)>=effect.height)
                throw std::runtime_error("effect line endpoint outside texture");
        effect.sources.push_back(entry);
    }
    const auto* palette=node.child("palette");
    if(!palette)throw std::runtime_error("effect palette missing");
    std::array<uint32_t,256> colors{};
    for(const auto& [kind,entry]:palette->orderedChildren()) {
        const int first=number(*entry,kind=="color" ? "index" : "startindex");
        const int last=kind=="color" ? first : number(*entry,"endindex");
        if(first<0 || last<first || last>=256)throw std::runtime_error("invalid effect palette range");
        if(kind=="color")colors[size_t(first)]=color(*entry,"color");
        else if(kind=="ramp")retailEffectColorRamp(colors,first,last,color(*entry,"startcolor"),color(*entry,"endcolor"));
        else throw std::runtime_error("unsupported effect palette section: "+std::string(kind));
    }
    result.palette=retailEffectPalette(colors,effect.intensity);
    return result;
}
} // namespace tak
