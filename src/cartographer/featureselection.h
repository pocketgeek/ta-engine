#pragma once
#include "tnt/tnt.h"
#include <algorithm>
#include <set>

namespace cart {
// Cell indices are transient. Clipboard names remain valid across documents.
struct FeatureSelection {
    struct Item {int x,z;std::string name;};
    std::set<int> cells;
    std::vector<Item> clipboard;
    bool dragging=false;
    int dragX=0,dragZ=0;
    static bool selectable(const tak::tnt::Map& map,int i) {
        return i>=0 && size_t(i)<map.features.size() && map.features[i]<0xFFFA && map.features[i]<map.featureNames.size();
    }
    void clear() {cells.clear();dragging=false;}
    void click(int i,bool append) {
        if(!append && !cells.contains(i))cells.clear();
        if(append && cells.contains(i))cells.erase(i);else cells.insert(i);
    }
    void box(const tak::tnt::Map& map,int x0,int z0,int x1,int z1,bool append) {
        if(!append)cells.clear();
        const int lx=std::max(0,std::min(x0,x1)),hx=std::min(map.width-1,std::max(x0,x1));
        const int lz=std::max(0,std::min(z0,z1)),hz=std::min(map.height-1,std::max(z0,z1));
        for(int z=lz;z<=hz;++z)for(int x=lx;x<=hx;++x) {
            const int i=z*map.width+x;if(selectable(map,i))cells.insert(i);
        }
    }
    void copy(const tak::tnt::Map& map) {
        clipboard.clear();for(int i:cells)if(selectable(map,i))clipboard.push_back({i%map.width,i/map.width,map.featureNames[map.features[i]]});
    }
    bool remove(tak::tnt::Map& map) {
        bool changed=false;
        for(int i:cells)if(selectable(map,i)) {map.features[i]=0xFFFF;changed=true;}
        clear();return changed;
    }
    bool move(tak::tnt::Map& map,int dx,int dz) {
        // Members erased since they were selected (Erase mode, Clear Area) leave the group.
        std::erase_if(cells,[&](int i){return !selectable(map,i);});
        if(cells.empty() || (!dx && !dz))return false;
        std::vector<std::pair<int,uint16_t>> moved;
        for(int i:cells) {
            const int x=i%map.width+dx,z=i/map.width+dz;
            if(x<0 || z<0 || x>=map.width || z>=map.height)return false;
            const int to=z*map.width+x;
            // Never erase another feature or a terrain marker by moving a group.
            if(map.features[to]!=0xFFFF && !cells.contains(to))return false;
            moved.push_back({to,map.features[i]});
        }
        for(int i:cells)map.features[i]=0xFFFF;
        cells.clear();for(auto [i,value]:moved) {map.features[i]=value;cells.insert(i);}
        return true;
    }
    bool drag(tak::tnt::Map& map,int x,int z) {
        if(!dragging || !move(map,x-dragX,z-dragZ))return false;
        dragX=x;dragZ=z;return true;
    }
    bool paste(tak::tnt::Map& map,int x,int z) {
        if(clipboard.empty())return false;
        std::vector<std::pair<int,uint16_t>> added;
        auto names=map.featureNames;
        for(const auto& item:clipboard) {
            const int px=x+item.x-clipboard.front().x,pz=z+item.z-clipboard.front().z;
            if(px<0 || pz<0 || px>=map.width || pz>=map.height)return false;
            const int i=pz*map.width+px;if(map.features[i]!=0xFFFF)return false;
            auto name=std::find(names.begin(),names.end(),item.name);
            if(name==names.end()) {
                if(names.size()>=0xFFFA)return false;
                names.push_back(item.name);name=names.end()-1;
            }
            added.push_back({i,uint16_t(name-names.begin())});
        }
        map.featureNames=std::move(names);clear();
        for(auto [i,value]:added) {map.features[i]=value;cells.insert(i);}
        return true;
    }
};
}
