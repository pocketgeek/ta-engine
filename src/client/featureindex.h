#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace tak {
// Broad-phase scenery culling in projected world coordinates. Features retain
// their original painter-list order; the caller still performs exact screen/fog
// tests. Rebuilt only when features are added or their footprint moves the anchor.
class FeatureIndex {
    struct Entry { int z,x; size_t index; };
    std::vector<Entry> entries_;
    std::vector<size_t> all_,visible_;
    int minX_=0,maxX_=0,minZ_=0,maxZ_=0;
    static int cell(float value) { return int(std::floor(value/1024.f)); }
public:
    template<class Point> void rebuild(size_t count,Point point) {
        entries_.clear();entries_.reserve(count);all_.resize(count);
        for(size_t i=0;i<count;++i) {
            const auto [x,z]=point(i);
            const int cx=cell(x),cz=cell(z);
            entries_.push_back({cz,cx,i});all_[i]=i;
            if(i==0){minX_=maxX_=cx;minZ_=maxZ_=cz;}
            else {minX_=std::min(minX_,cx);maxX_=std::max(maxX_,cx);
                  minZ_=std::min(minZ_,cz);maxZ_=std::max(maxZ_,cz);}
        }
        std::sort(entries_.begin(),entries_.end(),[](const Entry& a,const Entry& b) {
            return std::tie(a.z,a.x,a.index)<std::tie(b.z,b.x,b.index);
        });
    }
    std::span<const size_t> all() const { return all_; }
    std::span<const size_t> query(float left,float top,float right,float bottom) {
        visible_.clear();
        if(entries_.empty())return visible_;
        const int x0=std::max(minX_,cell(left)),x1=std::min(maxX_,cell(right));
        const int z0=std::max(minZ_,cell(top)),z1=std::min(maxZ_,cell(bottom));
        if(x0>x1 || z0>z1)return visible_;
        // At far zoom most scenery is on screen: avoid gathering/sorting it.
        if(entries_.size()<256 || (x0==minX_ && x1==maxX_ && z0==minZ_ && z1==maxZ_))return all_;
        for(int z=z0;z<=z1;++z) {
            auto it=std::lower_bound(entries_.begin(),entries_.end(),std::pair{z,x0},
                [](const Entry& e,const auto& key){return std::pair{e.z,e.x}<key;});
            for(;it!=entries_.end() && it->z==z && it->x<=x1;++it)visible_.push_back(it->index);
        }
        if(visible_.size()>all_.size()/2)return all_;
        std::sort(visible_.begin(),visible_.end());
        return visible_;
    }
};
}
