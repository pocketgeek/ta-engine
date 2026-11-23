#include "cartographer/regions.h"
#include "cartographer/triggers.h"
#include <algorithm>
#include <cctype>
#include <cmath>
namespace cart {
namespace {
std::string lower(std::string s) {for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;}
template<class F> void locations(tak::crt::Scenario& scenario,F fn) {
    for(auto& player:scenario.players)for(auto& group:player)for(bool action:{false,true}) {
        auto& rules=action?group.actions:group.conditions;
        const auto& defs=action?actionDefs():conditionDefs();
        for(auto& rule:rules)if(rule.opcode>=0 && size_t(rule.opcode)<defs.size())
            for(size_t i=0;i<defs[rule.opcode].params.size();++i)
                if(defs[rule.opcode].params[i]==PKind::Location)fn(rule.slot[i]);
    }
}
}
bool setRegion(tak::crt::Scenario& scenario,int index,tak::crt::Region region,int width,int height,std::string& error) {
    error.clear();
    if(index<-1 || index>=int(scenario.regions.size())) {error="Region no longer exists.";return false;}
    const auto name=lower(region.name);
    if(name.empty() || name=="anywhere" || name.size()>63 ||
       std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isspace(c);})) {
        error="Use a unique region name of 1 to 63 bytes, other than Anywhere.";return false;
    }
    for(int i=0;i<int(scenario.regions.size());++i)if(i!=index && lower(scenario.regions[i].name)==name) {
        error="That region name is already used.";return false;
    }
    if(region.x1>region.x2)std::swap(region.x1,region.x2);
    if(region.z1>region.z2)std::swap(region.z1,region.z2);
    if(region.x1<0 || region.z1<0 || region.x2>=width || region.z2>=height) {
        error="All region corners must be inside the map. Bounds include both corner cells.";return false;
    }
    if(index<0)scenario.regions.push_back(std::move(region));
    else {
        const auto old=lower(scenario.regions[index].name);
        if(scenario.regions[index].name!=region.name)
            locations(scenario,[&](std::string& value){if(lower(value)==old)value=region.name;});
        scenario.regions[index]=std::move(region);
    }
    return true;
}
bool removeRegion(tak::crt::Scenario& scenario,int index,std::string& error) {
    error.clear();
    if(index<0 || index>=int(scenario.regions.size())) {error="Select a region first.";return false;}
    const auto name=lower(scenario.regions[index].name);bool referenced=false;
    locations(scenario,[&](const std::string& value){referenced|=lower(value)==name;});
    if(referenced) {error="A scenario rule uses this region. Change its location before deleting the region.";return false;}
    scenario.regions.erase(scenario.regions.begin()+index);return true;
}
int RegionDrag::hit(const tak::crt::Region& r,float x,float z,float tolerance) {
    if(x<r.x1-tolerance || x>r.x2+1+tolerance || z<r.z1-tolerance || z>r.z2+1+tolerance)return None;
    int p=None;
    const float left=std::abs(x-r.x1),right=std::abs(x-r.x2-1);
    const float top=std::abs(z-r.z1),bottom=std::abs(z-r.z2-1);
    if(std::min(left,right)<=tolerance)p|=left<=right?Left:Right;
    if(std::min(top,bottom)<=tolerance)p|=top<=bottom?Top:Bottom;
    return p?p:Move;
}
bool RegionDrag::begin(const std::vector<tak::crt::Region>& regions,int preferred,float x,float z,
                       float tolerance,bool create,int width,int height) {
    cancel();if(width<1 || height<1)return false;
    grabX=int(std::floor(x));grabZ=int(std::floor(z));startX=x;startZ=z;
    if(create) {
        if(grabX<0 || grabX>=width || grabZ<0 || grabZ>=height)return false;
        int number=1;std::string name;
        do {name="Region "+std::to_string(number++);}
        while(std::any_of(regions.begin(),regions.end(),[&](const auto& r){return lower(r.name)==lower(name);}));
        original={name,grabX,grabZ,grabX,grabZ};part=Draw;
    } else {
        auto pick=[&](int i) {
            if(i<0 || i>=int(regions.size()))return false;
            auto r=regions[i];
            if(r.x1>r.x2)std::swap(r.x1,r.x2);
            if(r.z1>r.z2)std::swap(r.z1,r.z2);
            if(r.x1<0 || r.z1<0 || r.x2>=width || r.z2>=height)return false;
            const int p=hit(r,x,z,tolerance);if(!p)return false;
            index=i;part=p;original=r;return true;
        };
        if(!pick(preferred))for(int i=int(regions.size())-1;i>=0;--i)if(pick(i))break;
        if(part==None)return false;
    }
    preview=original;return true;
}
void RegionDrag::update(float x,float z,int width,int height) {
    if(!active() || width<1 || height<1)return;
    preview=original;
    if(part==Draw) {
        const int cx=std::clamp(int(std::floor(x)),0,width-1),cz=std::clamp(int(std::floor(z)),0,height-1);
        preview.x1=std::min(grabX,cx);preview.x2=std::max(grabX,cx);
        preview.z1=std::min(grabZ,cz);preview.z2=std::max(grabZ,cz);
    } else if(part==Move) {
        const int dx=std::clamp(int(std::floor(x))-grabX,-original.x1,width-1-original.x2);
        const int dz=std::clamp(int(std::floor(z))-grabZ,-original.z1,height-1-original.z2);
        preview.x1+=dx;preview.x2+=dx;preview.z1+=dz;preview.z2+=dz;
    } else {
        // Preserve the grab offset within the handle: a click without motion
        // must not resize, even at a low zoom where a handle spans several cells.
        const int dx=int(std::lround(x-startX)),dz=int(std::lround(z-startZ));
        if(part&Left)preview.x1=std::clamp(original.x1+dx,0,original.x2);
        if(part&Right)preview.x2=std::clamp(original.x2+dx,original.x1,width-1);
        if(part&Top)preview.z1=std::clamp(original.z1+dz,0,original.z2);
        if(part&Bottom)preview.z2=std::clamp(original.z2+dz,original.z1,height-1);
    }
}
}
