// Research-only dump of effective fields affected by the archived balance deltas.
// This deliberately does not claim to serialize every engine or retail rule.
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <iomanip>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

using Fields=std::map<std::pair<std::string,std::string>,std::string>;
template<class T> static std::string value(T v) {
    std::ostringstream s;s.imbue(std::locale::classic());
    if constexpr(std::is_enum_v<T>)s<<int(v);
    else if constexpr(std::is_floating_point_v<T>)s<<std::setprecision(std::numeric_limits<T>::max_digits10)<<v;
    else if constexpr(std::is_integral_v<T>)s<<+v;
    else s<<v;
    return s.str();
}
static std::string escape(const std::string& s) {
    if(s.empty())return "<empty>";
    std::string out;for(char c:s) {
        if(c=='\\')out+="\\\\";else if(c=='\t')out+="\\t";
        else if(c=='\r')out+="\\r";else if(c=='\n')out+="\\n";else out+=c;
    }return out;
}
static void weapon(Fields& fields,const std::string& id,const std::string& prefix,const tak::sim::Weapon& w) {
    auto add=[&](const char* key,auto v){fields[{prefix+key,id}]=value(v);};
#define W(member) add(#member,w.member)
    W(range);W(reload);W(switchReloadTicks);W(damage);W(projVel);W(subSteps);
    W(melee);W(aoe);W(edge);W(aimTol);W(ballistic);W(beam);W(straight);W(lightning);
    add("flameKind",int(w.flameKind));W(emitTime);W(kind);W(remote);W(particlesPerSec);
    W(turnRate);W(buildUp);W(buildUpTicks);W(decay);W(duration);W(durationTicks);
    W(variationTicks);W(maxVariation);W(variationTime);W(mindControl);W(minRange);
    W(noAir);W(manaCost);W(dmgType);W(status);W(statusDur);W(fireStarter);
    // Name prose is omitted; its effective downstream visual classifier remains.
    W(fx);W(explosionClass);W(waterExplosionClass);W(weaponArt);W(shotModel);
    W(soundHit);W(lightMap);W(shakeMag);
#undef W
    // Missing table entries mean fallback to this weapon's default damage, not zero.
    for(const auto& [category,amount]:w.dmgVs)
        fields[{prefix+"damageCategory/"+category,id}]=value(amount);
}
static Fields dump(const tak::hpi::Vfs& vfs,bool cb,const std::set<std::string>& builders) {
    tak::sim::TypeRegistry reg;tak::sim::setupRegistry(reg,vfs,cb);Fields result;
    if(reg.types().empty())throw std::runtime_error("no loaded unit types");
    for(const auto& [id,t]:reg.types()) {
        auto add=[&](const char* key,auto v){result[{std::string("unit/")+key,id}]=value(v);};
        add("present",true);
#define U(member) add(#member,t.member)
        U(buildCost);U(buildTime);U(healTime);U(workerTime);U(income);U(storage);
        U(maxMana);U(manaRegen);U(maxHp);U(sight);U(radar);U(buildDist);
        U(experiencePoints);U(leash);U(cruiseAlt);U(hoverAttackAltitude);
        U(turnRate);U(turnInPlaceRate);U(waterline);U(cantBeCaptured);
        U(cantBeFrozen);U(cantBeStoned);U(canTransport);U(floater);U(onOffable);
        U(transportCap);U(transportSizeCap);U(maxTransportSize);U(transportDist);
        U(corpse);U(stoneFeat);U(frozenFeat);U(damageCategory);U(bodyType);
        U(receivesWind);U(maxWaterDepth);U(minWaterDepth);U(maxWaterSlope);
        U(maxSlope);U(footX);U(footZ);U(domain);U(transportLandEligible);
        U(noShadow);U(defaultFire);U(hasExplodeAs);U(halfCellTicks);U(maxWeaponReloadMs);
        U(badSlope);U(badWaterSlope);U(badMaxWaterDepth);U(badMinWaterDepth);U(transportSize);
        add("isStructure",t.isStructure());
#undef U
        add("maxVel.fixed16",t.maxVel.v);add("accel.fixed16",t.accel.v);
        add("brake.fixed16",t.brake.v);add("waterMult.fixed16",t.waterMult.v);
        add("roadMult.fixed16",t.roadMult.v);
        for(size_t color=0;color<t.bloodColors.size();++color)
            result[{"unit/bloodColor/"+std::to_string(color+1),id}]=value(t.bloodColors[color]);
        for(size_t i=0;i<t.auras.size();++i) {
            const auto& a=t.auras[i];auto prefix="aura/"+std::to_string(i)+"/";
            result[{prefix+"kind",id}]=value(a.kind);result[{prefix+"amount",id}]=value(a.amount);
            result[{prefix+"radius",id}]=value(a.radius);result[{prefix+"affectsEnemy",id}]=value(a.affectsEnemy);
            result[{prefix+"edge",id}]=value(a.edge);
        }
        add("activeWeapons",t.weapons.size());
        for(size_t i=0;i<t.weapons.size();++i) {
            const auto native=t.weaponNativeSlotForLocal[i];
            weapon(result,id,"weapon"+std::to_string(native+1)+"/",t.weapons[i]);
        }
        weapon(result,id,"explodeAs/",t.explodeAs);
    }
    for(const auto& builder:builders) {
        const auto& menu=reg.buildable(builder);std::string text;
        for(const auto& id:menu){if(!text.empty())text+=",";text+=id;}
        result[{"menu/ordered",builder}]=text;
    }
    return result;
}
int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"usage: crusades_effective_audit RETAIL_ROOT\n";return 2;}
    SDL_SetMainReady();
    try {
        auto mounts=tak::hpi::mountRetailRoot(argv[1]);
        tak::hpi::Vfs vfs(&mounts,true);std::set<std::string> builders;
        if(vfs.list("unitscb").empty())throw std::runtime_error("no Crusades unit roster");
        // Retail builds every menu from canbuild/ in both modes (it never reads
        // canbuildcb/); Crusades changes the roster those menus are filtered by.
        const std::string prefix="canbuild";
        for(const auto& path:vfs.list(prefix)) {
            auto lower=[](std::string s) {
                std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});
                return s;
            };
            if(lower(std::filesystem::path(path).extension().string())!=".tdf")continue;
            std::vector<std::string> segments;
            for(const auto& part:std::filesystem::path(path)) {
                auto segment=part.string();
                if(!segment.empty() && segment!="/")segments.push_back(segment);
            }
            size_t i=0;while(i<segments.size() && lower(segments[i])!=prefix)++i;
            if(i+3!=segments.size())continue;
            builders.insert(lower(segments[i+1]));
        }
        const auto standard=dump(vfs,false,builders),crusades=dump(vfs,true,builders);
        std::set<std::pair<std::string,std::string>> keys;
        for(const auto& [key,v]:standard)keys.insert(key);
        for(const auto& [key,v]:crusades)keys.insert(key);
        // A unit absent from one roster (no unitscb FBI: retail has no fallback)
        // reports just its unit/present row, not every field as <absent>.
        auto present=[](const Fields& f,const std::string& id){return f.count({"unit/present",id})!=0;};
        size_t changed=0;std::cout<<"field\tid\tstandard\tcrusades\n";
        for(const auto& key:keys) {
            if(key.first!="unit/present" && !key.first.starts_with("menu/") &&
               present(standard,key.second)!=present(crusades,key.second))continue;
            auto a=standard.find(key),b=crusades.find(key);
            const std::string av=a==standard.end()?"<absent>":a->second;
            const std::string bv=b==crusades.end()?"<absent>":b->second;
            if(av==bv)continue;
            ++changed;
            std::cout<<escape(key.first)<<'\t'<<escape(key.second)<<'\t'<<escape(av)<<'\t'<<escape(bv)<<'\n';
        }
        std::cerr<<changed<<" changed effective fields from "<<keys.size()<<" loaded field entries\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"effective audit: "<<e.what()<<'\n';return 1;}
}
