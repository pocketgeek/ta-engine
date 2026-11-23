// Retail-backed contract checks: balance selection, scalar stats and whole menus.
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include "tdf/tdf.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

static std::string lower(std::string s) {
    for(auto& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static tak::tdf::Node read(const tak::hpi::Vfs& vfs,const std::string& path) {
    auto bytes=vfs.read(path);
    return tak::tdf::parseText(std::string(bytes.begin(),bytes.end()),path);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    SDL_SetMainReady();
    try {
        auto mounts=tak::hpi::mountRetailRoot(argv[1]);
        tak::hpi::Vfs vfs(&mounts,true);
        int failures=0,checks=0,units=0,menus=0,weapons=0,explosions=0,overrides=0;
        auto check=[&](bool ok,const std::string& context) {
            ++checks;if(!ok){++failures;std::cerr<<"FAIL "<<context<<'\n';}
        };
        // These are independent source contracts, not a serialization of UnitType.
        struct Scalar {const char* key;float tak::sim::UnitType::* member;double fallback;};
        const Scalar scalars[]={
            {"buildcost",&tak::sim::UnitType::buildCost,0},
            {"buildtime",&tak::sim::UnitType::buildTime,0},
            {"healtime",&tak::sim::UnitType::healTime,0},
            {"workertime",&tak::sim::UnitType::workerTime,1},
            {"mogriumincome",&tak::sim::UnitType::income,0},
            {"mogriumstorage",&tak::sim::UnitType::storage,0},
            {"maxmana",&tak::sim::UnitType::maxMana,0},
            {"manarechargerate",&tak::sim::UnitType::manaRegen,0},
        };
        struct Integer {const char* key;int32_t tak::sim::UnitType::* member;int fallback;};
        const Integer integers[]={
            {"maxdamage",&tak::sim::UnitType::maxHp,100},
            {"sightdistance",&tak::sim::UnitType::sight,180},
            {"radardistance",&tak::sim::UnitType::radar,0},
            {"builddistance",&tak::sim::UnitType::buildDist,0},
            {"experiencepoints",&tak::sim::UnitType::experiencePoints,666},
            {"maneuverleashlength",&tak::sim::UnitType::leash,0},
            {"cruisealt",&tak::sim::UnitType::cruiseAlt,0},
            {"turnrate",&tak::sim::UnitType::turnRate,500},
            {"waterline",&tak::sim::UnitType::waterline,0}
        };
        // Retail: the roster is ONE directory (units, or unitscb for Crusades, no
        // fallback); menus are always canbuild/ filtered by the roster.
        std::map<bool,std::set<std::string>> idsFor;std::set<std::string> builders;
        std::map<bool,std::map<std::string,std::string>> aliasesFor;
        check(!vfs.list("unitscb").empty(),"Crusades unit roster is present");
        for(bool cb:{false,true})for(const auto& path:vfs.list(cb?"unitscb":"units")) {
            if(!lower(path).ends_with(".fbi"))continue;
            auto doc=read(vfs,path);auto info=doc.child("unitinfo");
            if(!info)continue;
            auto stem=lower(std::filesystem::path(path).stem().string());
            if(lower(info->valueOr("objectname",stem))==stem)idsFor[cb].insert(stem);
            else aliasesFor[cb][stem]=lower(info->valueOr("objectname",stem));
        }
        std::map<std::string,std::vector<std::pair<double,std::string>>> sourceMenus;
        const std::string prefix="canbuild";
        for(const auto& path:vfs.list(prefix)) {
            auto key=lower(path);if(!key.ends_with(".tdf"))continue;
            auto slash=key.find('/',prefix.size()+1);
            if(slash==std::string::npos || key.find('/',slash+1)!=std::string::npos)continue;
            auto builder=key.substr(prefix.size()+1,slash-prefix.size()-1);
            auto id=std::filesystem::path(key).stem().string();
            auto doc=read(vfs,path);auto menu=doc.child("menu");
            const double none=9999999999.0;
            sourceMenus[builder].push_back({menu?menu->numberOr("priority",none):none,id});
            builders.insert(builder);
        }
        auto moveInfo=read(vfs,"gamedata/moveinfo.tdf");
        std::map<std::string,const tak::tdf::Node*> movement;
        for(const auto& [name,node]:moveInfo.orderedChildren())
            movement[lower(node->valueOr("name",""))]=node;
        for(bool cb:{false,true}) {
            tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,cb);
            std::string mode=cb?"Crusades ":"standard ";
            const auto& ids=idsFor[cb];const auto& aliases=aliasesFor[cb];
            check(registry.types().size()==ids.size(),mode+" complete canonical roster");
            for(const auto& [alias,id]:aliases) {
                check(ids.count(id)!=0,mode+alias+" canonical source exists");
                if(!ids.count(alias))check(registry.find(alias)==nullptr,mode+alias+" does not create an extra unit type");
            }
            for(const auto& id:ids) {
                std::string path=(cb?"unitscb/":"units/")+id+".fbi";
                if(!vfs.has(path))continue;
                auto doc=read(vfs,path);auto info=doc.child("unitinfo");
                if(!info || lower(info->valueOr("objectname",id))!=id)continue;
                ++units;auto type=registry.find(id);
                check(type!=nullptr,mode+id+" exists");if(!type)continue;
                for(auto field:scalars)
                    check(type->*(field.member)==float(info->numberOr(field.key,field.fallback)),mode+id+" "+field.key);
                for(auto field:integers)
                    check(type->*(field.member)==int32_t(info->numberOr(field.key,field.fallback)),mode+id+" "+field.key);
                using Type=tak::sim::UnitType;
                struct Flag {const char* key;bool Type::* member;};
                for(auto field:{Flag{"cantbecaptured",&Type::cantBeCaptured},
                    {"cantransport",&Type::canTransport},{"floater",&Type::floater},
                    {"noshadow",&Type::noShadow},{"onoffable",&Type::onOffable}})
                    check(type->*(field.member)==(info->numberOr(field.key,0)!=0),mode+id+" "+field.key);
                check(type->cantBeFrozen==(type->commander || info->numberOr("cantbefrozen",0)!=0),mode+id+" cantbefrozen");
                check(type->cantBeStoned==(type->commander || info->numberOr("cantbestoned",0)!=0),mode+id+" cantbestoned");
                for(auto [key,actual]:{std::pair{"transportcapacity",type->transportCap},
                    {"transportsizecapacity",type->transportSizeCap},{"transportsize",type->maxTransportSize},
                    {"transportdistance",type->transportDist},{"turninplacerate",type->turnInPlaceRate}})
                    check(actual==uint16_t(int32_t(info->numberOr(key,0))),mode+id+" "+key);
                check(type->hoverAttackAltitude==int32_t(info->numberOr("hoverattackaltitude",type->cruiseAlt)),mode+id+" hoverattackaltitude");
                struct FixedField {const char* key;tak::sim::Fixed Type::* member;double fallback;};
                for(auto field:{FixedField{"maxvelocity",&Type::maxVel,0},
                    {"acceleration",&Type::accel,.5},{"brakerate",&Type::brake,.5}}) {
                    auto expected=int32_t(info->numberOr(field.key,field.fallback)*65536.0);
                    check((type->*(field.member)).v==expected,mode+id+" "+field.key);
                }
                check(type->waterMult.v==int32_t(info->numberOr("watermultiplier",1)*65536.0),mode+id+" water multiplier");
                check(type->roadMult.v==(info->value("roadmultiplier")?int32_t(info->numberOr("roadmultiplier",1.2)*65536.0):0x13333),mode+id+" road multiplier");
                for(auto [key,actual]:{std::pair{"corpse",type->corpse},{"stone",type->stoneFeat},
                    {"frozen",type->frozenFeat},{"damagecategory",type->damageCategory}})
                    check(actual==lower(info->valueOr(key,"")),mode+id+" "+key);
                check(type->damageCategoryId==registry.categoryId(lower(info->valueOr("damagecategory",""))),mode+id+" interned damage category");
                const int standing=int(info->numberOr("standingunitorder",3))&3;
                const int fire=standing==0?0:standing==3?(int(info->numberOr("standingfireorder",2))&3):2;
                check(type->defaultFire==fire,mode+id+" default fire order ignores misspelled key");
                check(type->bodyType==lower(info->valueOr("bodytype","default")),mode+id+" bodytype");
                check(type->receivesWind==(info->numberOr("wind",0)!=0 || info->numberOr("windgenerator",0)!=0),mode+id+" wind callback gate");
                for(int color=0;color<3;++color) {
                    auto text=info->valueOr("bloodcolor"+std::to_string(color+1),"");
                    if(text.empty())continue;
                    int red=0,green=0,blue=0;
                    check(std::sscanf(text.c_str(),"%d %d %d",&red,&green,&blue)==3,mode+id+" complete blood RGB");
                    const auto expected=0xff000000u | (uint32_t(std::clamp(red,0,255))<<16)
                        | (uint32_t(std::clamp(green,0,255))<<8) | uint32_t(std::clamp(blue,0,255));
                    check(type->bloodColors[size_t(color)]==expected,mode+id+" bloodcolor"+std::to_string(color+1));
                }
                auto mc=movement.find(lower(info->valueOr("movementclass","")));
                if(mc!=movement.end()) {
                    const auto& source=*mc->second;
                    auto integer=[&](const char* key,int32_t got,int fallback) {
                        check(got==int32_t(source.numberOr(key,fallback)),mode+id+" movement "+key);
                    };
                    integer("maxwaterdepth",type->maxWaterDepth,255);
                    integer("minwaterdepth",type->minWaterDepth,0);
                    integer("maxwaterslope",type->maxWaterSlope,255);
                    integer("badminwaterdepth",type->badMinWaterDepth,32768);
                    integer("badmaxwaterdepth",type->badMaxWaterDepth,32768);
                    const int rawSlope=int(source.numberOr("maxslope",255));
                    check(type->badSlope==std::min(int(source.numberOr("badslope",rawSlope/2)),type->maxSlope),mode+id+" movement badslope");
                    check(type->badWaterSlope==std::min(int(source.numberOr("badwaterslope",type->maxWaterSlope/2)),type->maxWaterSlope),mode+id+" movement badwaterslope");
                    check(type->transportLandEligible==(source.numberOr("minwaterdepth",-10000)<0),mode+id+" movement transport land eligibility");
                    check(type->maxSlope==std::min(int32_t(source.numberOr("maxslope",255)),type->maxWaterSlope),mode+id+" movement maxslope");
                    check(type->footX==int32_t(source.numberOr("footprintx",info->numberOr("footprintx",1))),mode+id+" movement footprintx");
                    check(type->footZ==int32_t(source.numberOr("footprintz",info->numberOr("footprintz",1))),mode+id+" movement footprintz");
                }
                const auto movementName=lower(info->valueOr("movementclass",""));
                const auto domain=movementName.starts_with("water")?Type::Domain::Water:
                    movementName.starts_with("hover")?Type::Domain::Hover:Type::Domain::Ground;
                check(type->domain==domain,mode+id+" movement domain");
                auto transportedSize=uint16_t(int32_t(info->numberOr("transportedsize",0)));
                if(!transportedSize)transportedSize=uint16_t(type->footX*type->footZ);
                check(type->transportSize==transportedSize,mode+id+" passenger footprint cost");
                const int64_t bestSpeed=(int64_t(type->maxVel.v)*std::max({type->roadMult.v,type->waterMult.v,65536}))>>16;
                const int crossing=bestSpeed<=0?255:int(std::clamp<int64_t>((8ll<<16)/bestSpeed,1,255));
                check(type->halfCellTicks==crossing,mode+id+" native movement time scale");
                const auto* joy=info->child("adjustjoy");
                auto actualJoy=std::find_if(type->auras.begin(),type->auras.end(),[](const auto& aura){return aura.kind==tak::sim::Aura::Kind::Joy;});
                check((actualJoy!=type->auras.end())==(joy!=nullptr),mode+id+" joy aura presence");
                if(joy && actualJoy!=type->auras.end()) {
                    check(actualJoy->amount==float(joy->numberOr("adjustment",1)),mode+id+" joy adjustment");
                    check(actualJoy->radius==int32_t(joy->numberOr("radius",200)),mode+id+" joy radius");
                    check(actualJoy->affectsEnemy==(joy->numberOr("affectsenemy",0)!=0),mode+id+" joy enemy");
                    check(actualJoy->edge==float(joy->numberOr("edgeeffectiveness",1)),mode+id+" joy edge");
                }
                auto checkWeapon=[&](const tak::tdf::Node& source,const tak::sim::Weapon& actual,
                                     const std::string& label) {
                    auto context=mode+id+" "+label+" ";
                    auto integer=[&](const char* key,int32_t got,int fallback=0) {
                        check(got==int32_t(source.numberOr(key,fallback)),context+key);
                    };
                    auto scalar=[&](const char* key,float got,double fallback=0) {
                        check(got==float(source.numberOr(key,fallback)),context+key);
                    };
                    integer("range",actual.range);
                    integer("areaofeffect",actual.aoe);
                    integer("minrange",actual.minRange);
                    integer("emittime",actual.emitTime);
                    integer("maxvariation",actual.maxVariation);
                    integer("particlespersecond",actual.particlesPerSec);
                    integer("shakemagnitude",actual.shakeMag);
                    check(actual.aimTol==int32_t(source.numberOr("aimtolerance",
                        source.numberOr("aimtolerence",1024))),context+"aimtolerance");
                    scalar("reloadtime",actual.reload,1);
                    scalar("weaponvelocity",actual.projVel);
                    scalar("manapershot",actual.manaCost);
                    scalar("builduptime",actual.buildUp);
                    scalar("decaytime",actual.decay);
                    scalar("duration",actual.duration);
                    scalar("variationtime",actual.variationTime);
                    check(actual.edge==float(source.numberOr("edgeeffectiveness",
                        source.numberOr("edgeeffectivness",1))),context+"edgeeffectiveness");
                    check(actual.noAir==(source.numberOr("noairweapon",0)!=0),context+"noairweapon");
                    check(actual.fireStarter==(source.numberOr("firestarter",0)!=0),context+"firestarter");
                    for(auto [key,got]:{std::pair{"explosionclass",actual.explosionClass},
                        {"waterexplosionclass",actual.waterExplosionClass},{"weaponart",actual.weaponArt}})
                        check(got==lower(source.valueOr(key,"")),context+key);
                    check(actual.soundHit==lower(source.valueOr("soundhitclass",
                        source.valueOr("soundhit",""))),context+"impact sound");
                    auto model=lower(source.valueOr("model",""));
                    if(model.ends_with(".3do"))model.resize(model.size()-4);
                    check(actual.shotModel==model,context+"model");
                    auto damageType=lower(source.valueOr("damagetype","normal"));
                    int expectedDamageType=damageType=="fire"?2:damageType=="explosion"?3:damageType=="paralyzer"?4:1;
                    check(actual.dmgType==expectedDamageType,context+"damagetype");
                    const auto family=lower(source.valueOr("type",""));
                    const auto subtype=lower(source.valueOr("subtype",""));
                    using Kind=tak::sim::Weapon::Kind;
                    auto expectedKind=subtype=="dropped"?Kind::Dropped:family=="guided"?Kind::Guided:
                        family=="remote effect"?Kind::Remote:family=="wandering"?Kind::Wandering:Kind::Normal;
                    check(actual.kind==expectedKind,context+"weapon family");
                    check(actual.melee==(family=="melee"),context+"melee");
                    check(actual.ballistic==(family=="ballistic"),context+"ballistic");
                    check(actual.beam==(family=="line of sight"),context+"line of sight");
                    check(actual.mindControl==(subtype=="mindcontrol"),context+"mindcontrol");
                    check(actual.name==source.valueOr("name",""),context+"authored name");
                    // The changed name is consumed by today's visual fallback;
                    // this checks that branch, not retail-rendering equivalence.
                    if(id=="vercen" && label=="weapon1")
                        check(actual.fx==(cb?tak::sim::WeaponFx::Fire:tak::sim::WeaponFx::Arrow),context+"changed-name visual fallback");
                    using Remote=tak::sim::Weapon::RemoteKind;
                    auto expectedRemote=subtype=="earthquake"?Remote::Earthquake:subtype=="hailstorm"?Remote::Hailstorm:
                        subtype=="mindcontrol"?Remote::MindCtl:subtype=="turntofrozen"?Remote::Freeze:Remote::Plain;
                    check(actual.remote==expectedRemote,context+"remote subtype");
                    using Status=tak::sim::Weapon::Status;
                    auto expectedStatus=subtype=="turntofrozen"?Status::Frozen:subtype=="turntostone"?Status::Stoned:
                        damageType=="paralyzer"?Status::Paralyzed:Status::None;
                    check(actual.status==expectedStatus,context+"status dispatch");
                    if(expectedStatus!=Status::None)
                        check(actual.statusDur==float(source.numberOr("duration",5)),context+"status duration");
                    check(actual.lightning==(family=="line of sight" && subtype=="lightning"),context+"lightning dispatch");
                    int expectedFlame=-1;
                    if(family=="line of sight")expectedFlame=subtype=="fire"?0:subtype=="bluefire"?1:subtype=="dieselflame"?2:-1;
                    check(actual.flameKind==expectedFlame,context+"flame dispatch");
                    const auto light=lower(source.valueOr("lightmap",""));
                    check(actual.lightMap==(light=="small"?1:light=="medium"?2:light=="large"?3:0),context+"lightmap");
                    check(actual.buildUpTicks==int32_t(source.numberOr("builduptime",0)*30),context+"buildup ticks");
                    check(actual.durationTicks==int32_t(source.numberOr("duration",0)*30),context+"duration ticks");
                    check(actual.variationTicks==int32_t(source.numberOr("variationtime",0)*30),context+"variation ticks");
                    constexpr float radiansPerDegree=3.14159265358979f/180.0f;
                    check(actual.turnRate==float(source.numberOr("turnrate",0))*radiansPerDegree,context+"turnrate conversion");
                    auto damage=source.child("damage");
                    check(actual.damage==(damage?uint16_t(int32_t(damage->numberOr("default",0))):0),context+"default damage");
                    std::map<std::string,int32_t> expected;
                    if(damage)for(const auto& [category,value]:damage->values)
                        if(category!="default") {
                            volatile double product=double(actual.damage)*damage->numberOr(category,0);
                            expected[category]=int32_t(product);
                        }
                    check(actual.dmgVs==expected,context+"complete category damage table");
                    for(const auto& [category,value]:expected) {
                        tak::sim::UnitType target;target.damageCategory=category;
                        target.damageCategoryId=registry.categoryId(category);
                        check(actual.damageVs(&target)==float(value),context+category+" effective damage");
                        auto uninterned=actual;uninterned.dmgVsIds.clear();
                        check(uninterned.damageVs(&target)==actual.damageVs(&target),context+category+" interned parity");
                        ++overrides;
                    }
                    tak::sim::UnitType target;target.damageCategory="__audit_unmatched__";
                    check(actual.damageVs(&target)==actual.damage,context+"unmatched category fallback");
                    for(const auto& [targetId,targetType]:registry.types()) {
                        auto found=expected.find(targetType.damageCategory);
                        float expectedDamage=found==expected.end()?float(actual.damage):float(found->second);
                        check(actual.damageVs(&targetType)==expectedDamage,context+"actual target "+targetId);
                    }
                };
                size_t localSlot=0;int maxReloadMs=0;
                for(size_t slot=0;slot<3;++slot) {
                    auto weapon=doc.child("weapon"+std::to_string(slot+1));
                    if(!weapon)continue;
                    maxReloadMs=std::max(maxReloadMs,int(uint16_t(int32_t(weapon->numberOr("reloadtime",0)*30)))*1000/30);
                    auto damage=weapon->child("damage");
                    if(!damage || uint16_t(int32_t(damage->numberOr("default",0)))==0)continue;
                    ++weapons;
                    check(localSlot<type->weapons.size(),mode+id+" weapon slot");
                    if(localSlot>=type->weapons.size())continue;
                    check(type->weaponNativeSlotForLocal[localSlot]==slot,mode+id+" native slot mapping");
                    checkWeapon(*weapon,type->weapons[localSlot++],"weapon"+std::to_string(slot+1));
                }
                check(type->maxWeaponReloadMs==maxReloadMs,mode+id+" maximum native-slot reload callback");
                check(localSlot==type->weapons.size(),mode+id+" complete active weapon roster");
                const auto* explosion=doc.child("explodeas");
                const auto* explosionDamage=explosion?explosion->child("damage"):nullptr;
                const bool expectedExplosion=explosionDamage && uint16_t(int32_t(explosionDamage->numberOr("default",0)))>0;
                check(type->hasExplodeAs==expectedExplosion,mode+id+" active death explosion");
                if(explosion) {
                    ++explosions;checkWeapon(*explosion,type->explodeAs,"explodeas");
                }

            }
            for(const auto& builder:builders) {
                std::vector<std::string> expected;
                const auto* owner=registry.find(builder);
                if(owner && owner->isBuilder) {
                    auto entries=sourceMenus[builder];
                    std::stable_sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.first<b.first;});
                    for(const auto& [priority,id]:entries)if(registry.find(id))expected.push_back(id);
                }
                check(registry.buildable(builder)==expected,mode+builder+" complete ordered menu");++menus;
            }
        }
        check(units>0 && menus>0 && weapons>0 && explosions>0 && overrides>0,"nonempty unit/menu/weapon/explosion/category coverage");
        std::cout<<checks<<" checks, "<<units<<" unit-mode cases, "<<menus<<" builder-mode menus, "<<weapons<<" weapons, "<<explosions<<" explosions, "<<overrides<<" category overrides, "<<failures<<" failures\n";
        return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
