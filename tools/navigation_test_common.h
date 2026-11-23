#pragma once

#include "sim/sim.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace navigation_test {
using namespace tak::sim;
inline void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
inline PathfindingMode mode(std::string_view value) {
    if(value=="retail")return PathfindingMode::Retail;
    if(value=="legion")return PathfindingMode::Legion;
    throw std::runtime_error("mode must be retail or legion");
}
inline UnitType mover(int kind=0,bool boat=false) {
    UnitType type{};type.id=type.name="navigation-mover-"+std::to_string(kind);
    type.canMove=true;type.maxHp=100;type.footX=type.footZ=2;type.sight=4096;
    type.maxVel=Fixed::raw(std::array{117964,65536,91750}[size_t(kind%3)]);
    type.accel=type.brake=Fixed::fromInt(10);
    type.turnRate=type.turnInPlaceRate=std::array{2500,2200,2400}[size_t(kind%3)];
    type.halfCellTicks=std::array{3,6,4}[size_t(kind%3)];type.buildTime=1;
    if(boat){type.domain=UnitType::Domain::Water;type.floater=true;type.minWaterDepth=4;type.maxWaterDepth=255;}
    return type;
}
inline void setup(World& world,PathfindingMode selected,bool serial=true,bool boat=false,int cells=256) {
    world.setSerialThreads(serial);world.setPathService(true);
    world.setPathfindingMode(selected);
    world.setTerrain(std::vector<uint8_t>(size_t(cells)*cells,boat?20:100),cells,cells,64);
    world.setMapPlacementFeatures(std::vector<uint16_t>(size_t(cells)*cells,0xffff),{});
}
inline void known(World& world) {
    world.updateNavigationExploration();
    auto& exploration=world.editNavigationExploration();
    std::fill(exploration.begin(),exploration.end(),0xffff);
}
inline bool finished(const World& world,const std::vector<int>& ids) {
    return std::all_of(ids.begin(),ids.end(),[&](int id){const auto* u=world.unit(id);return u&&u->alive()&&u->orders.empty();});
}
inline void legal(const World& world,const std::vector<int>& ids) {
    for(int id:ids){const auto* u=world.unit(id);require(u&&u->alive(),"cohort member disappeared");
        require(world.mobilePlacement(*u,footprintOrigin(u->x,u->type->footX),footprintOrigin(u->z,u->type->footZ),false),
            "cohort overlaps a body or blocked terrain");}
}
inline void compact(const World& world,const std::vector<int>& ids,int goalX,int goalZ,int radius) {
    for(int id:ids){const auto& u=*world.unit(id);const int64_t dx=u.x.floorInt()-goalX,dz=u.z.floorInt()-goalZ;
        if(dx*dx+dz*dz>int64_t(radius)*radius)std::printf("far id=%d position=%.2f,%.2f goal=%d,%d orders=%zu\n",id,u.x.toFloat(),u.z.toFloat(),goalX,goalZ,u.orders.size());
        require(dx*dx+dz*dz<=int64_t(radius)*radius,"cohort retired its orders outside the destination area");}
}
inline void rest(World& world,const std::vector<int>& ids,int ticks=120) {
    for(int i=0;i<300;++i)world.tick(1.f/30);
    std::vector<std::pair<Fixed,Fixed>> positions;
    for(int id:ids){const auto& u=*world.unit(id);positions.emplace_back(u.x,u.z);}
    for(int tick=0;tick<ticks;++tick){world.tick(1.f/30);for(size_t i=0;i<ids.size();++i){const auto& u=*world.unit(ids[i]);
        require(std::pair{u.x,u.z}==positions[i],"completed arrival resumed moving without a new command");}}
    legal(world,ids);
}
inline int middleWidth(const World& world,const std::vector<int>& ids) {
    std::vector<int> zs;for(int id:ids)zs.push_back(world.unit(id)->z.floorInt());std::sort(zs.begin(),zs.end());
    return zs[zs.size()*3/4]-zs[zs.size()/4];
}
}
