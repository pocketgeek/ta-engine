#include "cartographer/overlay.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <stdexcept>
namespace cart {
TerrainOverlay terrainOverlay(const tak::tnt::Map& map,const tak::sim::TypeRegistry& registry,
    const tak::hpi::Vfs& vfs,OverlayKind kind,const std::string& unitType,const std::vector<PlacedUnit>& occupants,const std::atomic_bool* cancel) {
    const auto cancelled=[&] {return cancel && cancel->load(std::memory_order_relaxed);};
    if(cancelled())return {};
    if(map.width<=0 || map.height<=0 || map.heights.size()!=size_t(map.width)*map.height)
        throw std::runtime_error("Invalid terrain dimensions");
    TerrainOverlay out;out.width=map.width;out.height=map.height;out.rgba.resize(map.heights.size()*4);
    const auto* type=registry.find(unitType);
    const bool classify=kind==OverlayKind::Movement || kind==OverlayKind::Buildability;
    if(classify && !type)throw std::runtime_error("Choose a unit type in the Units palette first.");
    std::unique_ptr<tak::sim::World> world;
    if(classify) {
        world=std::make_unique<tak::sim::World>();
        world->setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
        world->buildNavClasses(registry);tak::sim::registerMapFeatures(*world,map,vfs,&registry);
        if(kind==OverlayKind::Buildability) {
            world->setPlayerCount(8);
            for(const auto& occupant:occupants) {
                if(cancelled())return {};
                auto name=occupant.type;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
                const auto* placed=registry.find(name);
                if(placed && std::isfinite(occupant.x) && std::isfinite(occupant.z) && occupant.x>=0 && occupant.z>=0 && occupant.x<map.width*16.f && occupant.z<map.height*16.f)
                    world->spawn(placed,occupant.x,occupant.z,{},std::clamp(occupant.player,0,7));
            }
        }
        world->navFor(type).ensureClearance();
        out.legend=type->name+": GREEN=allowed RED=blocked "+(kind==OverlayKind::Buildability?"(terrain/features/preplaced units)":"(terrain/features only)");
        if(type->canFly && kind==OverlayKind::Movement)out.legend+="; AIR TRANSIT, not landing";
    } else out.legend=kind==OverlayKind::WaterDepth?"WATER DEPTH: brighter blue=deeper (corner minimum)":"SLOPE: darker green=flat, brighter orange=steep (corner spread)";
    std::vector<uint8_t> placement;
    if(kind==OverlayKind::Buildability) {
        placement=world->placementCells(type,cancel);
        if(placement.empty())return {};
    }
    for(int z=0;z<map.height;++z) {
      if(cancelled())return {};
      for(int x=0;x<map.width;++x) {
        const auto height=[&](int cx,int cz){return int(map.heights[size_t(std::min(cz,map.height-1))*map.width+std::min(cx,map.width-1)]);};
        const int a=height(x,z),b=height(x+1,z),c=height(x,z+1),d=height(x+1,z+1);
        const int low=std::min({a,b,c,d}),high=std::max({a,b,c,d});
        auto* pixel=out.rgba.data()+(size_t(z)*map.width+x)*4;
        if(classify) {
            const bool allowed=kind==OverlayKind::Buildability?placement[size_t(z)*map.width+x]:
                (type->canFly || world->navFor(type).fits(x,z,std::max(type->footX,type->footZ)));
            pixel[0]=allowed?30:235;pixel[1]=allowed?210:35;pixel[2]=45;pixel[3]=100;
        } else if(kind==OverlayKind::WaterDepth) {
            const int depth=std::max(0,map.seaLevel-low);
            pixel[0]=20;pixel[1]=uint8_t(std::min(220,40+depth));pixel[2]=uint8_t(std::min(255,80+depth*2));pixel[3]=depth?150:0;
        } else {
            const int spread=high-low;
            pixel[0]=uint8_t(std::min(255,spread*8));pixel[1]=uint8_t(std::max(35,180-spread*3));pixel[2]=30;pixel[3]=125;
        }
      }
    }
    return out;
}
}
