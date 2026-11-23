#pragma once
#include "cartographer/units.h"
#include <algorithm>
#include <set>

namespace cart {
// Editor indices are transient; clear after document replacement or deletion.
struct UnitSelection {
    std::set<int> indices;
    std::vector<PlacedUnit> clipboard;
    std::vector<std::pair<int,std::pair<float,float>>> dragOrigins;
    float dragX=0,dragZ=0;
    bool contains(int i) const {return indices.count(i)!=0;}
    void click(int i,bool append) {
        if(!append && !contains(i))indices.clear();
        if(i>=0) {if(append && contains(i))indices.erase(i);else indices.insert(i);}
    }
    void box(const std::vector<PlacedUnit>& units,float x0,float z0,float x1,float z1,bool append) {
        if(!append)indices.clear();
        const auto [lx,hx]=std::minmax(x0,x1);const auto [lz,hz]=std::minmax(z0,z1);
        for(int i=0;i<int(units.size());++i)if(units[i].x>=lx && units[i].x<=hx && units[i].z>=lz && units[i].z<=hz)indices.insert(i);
    }
    void beginDrag(const std::vector<PlacedUnit>& units,float x,float z) {
        dragOrigins.clear();dragX=x;dragZ=z;
        for(int i:indices)if(i>=0 && i<int(units.size()))dragOrigins.push_back({i,{units[i].x,units[i].z}});
    }
    bool drag(std::vector<PlacedUnit>& units,float x,float z,float width,float height) const {
        float dx=x-dragX,dz=z-dragZ;
        if(dragOrigins.empty())return false;
        float lowX=8-dragOrigins.front().second.first,highX=width-8-dragOrigins.front().second.first;
        float lowZ=8-dragOrigins.front().second.second,highZ=height-8-dragOrigins.front().second.second;
        for(const auto& [i,p]:dragOrigins) {
            lowX=std::max(lowX,8-p.first);highX=std::min(highX,width-8-p.first);
            lowZ=std::max(lowZ,8-p.second);highZ=std::min(highZ,height-8-p.second);
        }
        if(lowX>highX || lowZ>highZ)return false;
        dx=std::clamp(dx,lowX,highX);dz=std::clamp(dz,lowZ,highZ);
        bool changed=false;
        for(const auto& [i,p]:dragOrigins)if(i>=0 && i<int(units.size())) {
            changed|=units[i].x!=p.first+dx || units[i].z!=p.second+dz;
            units[i].x=p.first+dx;units[i].z=p.second+dz;
        }
        return changed;
    }
    bool remove(std::vector<PlacedUnit>& units) {
        bool changed=false;
        for(auto i=indices.rbegin();i!=indices.rend();++i)if(*i>=0 && *i<int(units.size())) {units.erase(units.begin()+*i);changed=true;}
        indices.clear();dragOrigins.clear();return changed;
    }
    void copy(const std::vector<PlacedUnit>& units) {
        clipboard.clear();for(int i:indices)if(i>=0 && i<int(units.size()))clipboard.push_back(units[i]);
    }
    bool paste(std::vector<PlacedUnit>& units,float x,float z,float width,float height) {
        if(clipboard.empty())return false;
        float minX=clipboard[0].x,maxX=minX,minZ=clipboard[0].z,maxZ=minZ;
        for(const auto& unit:clipboard) {minX=std::min(minX,unit.x);maxX=std::max(maxX,unit.x);minZ=std::min(minZ,unit.z);maxZ=std::max(maxZ,unit.z);}
        if(maxX-minX>width-16 || maxZ-minZ>height-16)return false;
        const float dx=std::clamp(x-clipboard[0].x,8-minX,width-8-maxX);
        const float dz=std::clamp(z-clipboard[0].z,8-minZ,height-8-maxZ);
        indices.clear();
        for(auto unit:clipboard) {
            unit.x+=dx;unit.z+=dz;unit.name.clear(); // copies start without the original display label
            indices.insert(int(units.size()));units.push_back(std::move(unit));
        }
        return true;
    }
};
} // namespace cart
