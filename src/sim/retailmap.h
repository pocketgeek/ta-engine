#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tak::sim {

struct RetailMapBoundaryCell {
    uint16_t feature=0xffff;
    uint8_t height=0,low=0;
};

struct RetailMapFeatureType {
    std::string name;
    int footX=1,footZ=1;
    bool blocking=false,indestructible=false;
    bool clearable=false; // No indestructible blocker in the dead/burnt replacement graph.
    uint8_t projectileHeight=0; // Feature TDF height, stored as a byte by retail.
    // TDF category trees, plant, plants or rocks. Legion only (not retail): a
    // lone one is no wall end to its pinwheel (LegionNavigator, wallOrigin).
    bool treeOrRock=false;
};

// Cell +7: the owning player of the feature anchored there. 50f78c starts
// every cell at 0xa (world, no player: slots are 0..9); 495360 writes its
// fifth argument at the ANCHOR only (4958ec), on success. Every map-feature
// caller passes 0xa; corpse spawn 512ee0 passes the dying unit's owner.
// Tails and removal leave it alone.
inline constexpr uint8_t kRetailWorldFeatureOwner=0xa;

struct RetailMapFeatureCell : RetailMapBoundaryCell {
    uint8_t backX=0,backZ=0;
    uint8_t owner=kRetailWorldFeatureOwner;
};

// Remove an anchor and its footprint tails. Boundary markers survive removal.
template<class CellAt>
void retailMapRemove(int x,int z,const RetailMapFeatureType& type,CellAt cellAt) {
    cellAt(x,z).feature=0xffff;
    for (int dz=0;dz<type.footZ;++dz) for (int dx=0;dx<type.footX;++dx) {
        auto& cell=cellAt(x+dx,z+dz);
        if (cell.feature==0xfffe) cell.feature=0xffff;
    }
}

// 495360: intersecting removable features disappear in scan order, even if a
// later obstruction refuses the new footprint. Removed reports each old anchor.
template<class CellAt,class Removed>
bool retailMapInstall(int width,int height,int x,int z,uint16_t index,
                      const std::vector<RetailMapFeatureType>& types,
                      CellAt cellAt,Removed removed,uint8_t owner=kRetailWorldFeatureOwner) {
    const auto& type=types.at(index);
    if (type.name.empty() || type.footX<=0 || type.footZ<=0 || x<0 || z<0 ||
        x+type.footX>width || z+type.footZ>height) return false;
    for (int dz=0;dz<type.footZ;++dz) for (int dx=0;dx<type.footX;++dx) {
        auto& cell=cellAt(x+dx,z+dz);
        if (cell.feature==0xffff) continue;
        const int ox=x+dx-(cell.feature==0xfffe?cell.backX:0);
        const int oz=z+dz-(cell.feature==0xfffe?cell.backZ:0);
        const auto previous=cellAt(ox,oz).feature;
        if (previous>=types.size() || types[previous].indestructible) return false;
        retailMapRemove(ox,oz,types[previous],cellAt);
        removed(ox,oz,previous);
    }
    cellAt(x,z).feature=index;cellAt(x,z).owner=owner;
    for (int dz=0;dz<type.footZ;++dz) for (int dx=0;dx<type.footX;++dx) {
        if (!dx && !dz) continue;
        auto& cell=cellAt(x+dx,z+dz);
        cell.feature=0xfffe;cell.backX=uint8_t(dx);cell.backZ=uint8_t(dz);
    }
    return true;
}

// The raw TNT features that survive install: raw[i] where the installed plane
// still anchors it, else 0xffff (0xfffb/0xfffc markers kept). Retail keeps no
// raw list past the loader (50f948 frees it), so a feature removed by a later
// overlap (or refused) is drawn, blocked, counted and picked by nothing
// (4fc8d9 draws only installed cells). CellAt reads the installed plane.
template<class CellAt>
std::vector<uint16_t> retailSurvivingMapFeatures(int width,int height,
                                                 const std::vector<uint16_t>& raw,CellAt cellAt) {
    std::vector<uint16_t> out(raw.size(),0xffff);
    for (int z=0;z<height;++z) for (int x=0;x<width;++x) {
        const size_t i=size_t(z)*width+x;
        const uint16_t v=raw.at(i);
        if (v==0xfffb || v==0xfffc || (v<0xfffa && cellAt(x,z).feature==v)) out[i]=v;
    }
    return out;
}

// Pass 2 of the map loader (50f7ff..50f898) on a scratch plane: the same
// survivors World::installedMapFeatures() holds, for callers with no World
// (lobby previews, map tools). raw is width*height TNT feature indices.
inline std::vector<uint16_t> retailInstalledMapFeatures(int width,int height,
        const std::vector<uint16_t>& raw,const std::vector<RetailMapFeatureType>& types) {
    if (width<=0 || height<=0 || raw.size()!=size_t(width)*height) return raw;
    std::vector<RetailMapFeatureCell> cells(raw.size());
    auto cell=[&](int x,int z)->RetailMapFeatureCell& { return cells[size_t(z)*width+x]; };
    for (size_t i=0;i<raw.size();++i) if (raw[i]==0xfffc) cells[i].feature=0xfffc;
    for (int z=0;z<height;++z) for (int x=0;x<width;++x) {
        const uint16_t index=raw[size_t(z)*width+x];
        if (index<types.size()) retailMapInstall(width,height,x,z,index,types,cell,[](int,int,uint16_t){});
    }
    return retailSurvivingMapFeatures(width,height,raw,cell);
}

// 50eef0: mark the projected map boundary after feature installation. Named
// feature anchors and existing special markers survive; footprint tails do not.
// CellAt returns a mutable reference. The caller supplies valid map dimensions.
template<class CellAt>
void retailMapBoundary(int width,int height,int pixelHeight,int sea,bool blockWater,
                       CellAt cellAt) {
    auto mark=[&](int x,int z) {
        auto& cell=cellAt(x,z);
        if (cell.feature==0xffff || cell.feature==0xfffe) cell.feature=0xfffd;
    };
    for (int z=0;z<height;++z) {
        mark(width-2,z);
        mark(width-1,z);
    }
    for (int x=0;x<width;++x)
        for (int z=0;z*16-int(cellAt(x,z).height)/2<0;++z) mark(x,z);
    for (int x=0;x<width;++x) {
        int z=height-1;
        while (z*16-int(cellAt(x,z).height)/2>pixelHeight-128) {
            mark(x,z-1);
            --z;
        }
    }
    if (blockWater)
        for (int z=0;z<height;++z) for (int x=0;x<width;++x)
            if (int(cellAt(x,z).low)<=sea) mark(x,z);
}

} // namespace tak::sim
