#include "cartographer/validation.h"
#include "cartographer/aiprofiles.h"
#include "cartographer/triggers.h"
#include "terrain/terrain.h"
#include "hpi/hpi.h"
#include <cstdio>
#include "sim/matchsetup.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <cmath>
#include <memory>

namespace cart {
namespace {
std::string folded(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));});
    return value;
}
}
std::vector<MapIssue> validateTerrainResources(const tak::tnt::Map& map,const tak::hpi::Vfs& vfs) {
    std::vector<MapIssue> issues;
    const auto count=size_t(std::max(0,map.blocksX))*std::max(0,map.blocksY);
    if(map.blocksX<=0 || map.blocksY<=0 || map.blocksX!=map.width/2 || map.blocksY!=map.height/2 ||
       map.tileKeys.size()!=count || map.tileCols.size()!=count || map.tileRows.size()!=count) {
        issues.push_back({MapIssue::Severity::Error,"Invalid terrain tile dimensions or key/column/row arrays"});return issues;
    }
    struct Use {size_t first=0,count=0;};std::map<uint32_t,Use> used;
    for(size_t i=0;i<count;++i) {auto& use=used[map.tileKeys[i]];if(use.count++==0)use.first=i;}
    tak::terrain::Compositor compositor(vfs);
    for(const auto& [key,use]:used) {
        try {compositor.sectionImage(key,map.stockTerrain);}
        catch(const std::exception& error) {
            char id[16];std::snprintf(id,sizeof id,"%08x",key);
            issues.push_back({MapIssue::Severity::Error,"Unreadable terrain image terrain/"+std::string(id)+".jpg ("+
                std::to_string(use.count)+" blocks): "+error.what(),float(use.first%map.blocksX)*32+16,float(use.first/map.blocksX)*32+16});
        }
        // Decoding is once per resource; do not retain every section on large maps.
        compositor.clear();
    }
    return issues;
}
std::vector<MovementRegion> movementRegions(const tak::sim::NavGrid& grid,int footprint) {
    footprint=std::clamp(footprint,1,15);
    const int width=grid.width(),height=grid.height();grid.ensureClearance();
    std::vector<uint8_t> cells(size_t(width)*height);
    for(int z=0;z<height;++z)for(int x=0;x<width;++x)cells[size_t(z)*width+x]=grid.fits(x,z,footprint)?1:0;
    std::vector<MovementRegion> regions;std::vector<int> pending;
    for(int seed=0;seed<int(cells.size());++seed)if(cells[seed]==1) {
        MovementRegion region{0,seed%width,seed/width};pending.push_back(seed);cells[seed]=2;
        while(!pending.empty()) {
            const int cell=pending.back();pending.pop_back();++region.cells;
            const int x=cell%width,z=cell/width;
            // Engine components forbid diagonal corner cutting: every allowed
            // diagonal has an orthogonal route, so four neighbours give exactly
            // the same connectivity without reproducing the path search.
            auto visit=[&](int next) {if(cells[next]==1) {cells[next]=2;pending.push_back(next);}};
            if(x>0)visit(cell-1);
            if(x+1<width)visit(cell+1);
            if(z>0)visit(cell-width);
            if(z+1<height)visit(cell+width);
        }
        regions.push_back(region);
    }
    std::stable_sort(regions.begin(),regions.end(),[](const auto& a,const auto& b){return a.cells>b.cells;});
    return regions;
}
std::vector<MapIssue> validateRuleOperands(bool action,const tak::crt::Rule& rule,
    const tak::crt::Scenario& scenario,const tak::sim::TypeRegistry& registry) {
    std::vector<MapIssue> issues;
    auto issue=[&](std::string text,bool error=true) {issues.push_back({error?MapIssue::Severity::Error:MapIssue::Severity::Warning,std::move(text)});};
    const auto& definitions=action?actionDefs():conditionDefs();
    if(rule.opcode<0 || size_t(rule.opcode)>=definitions.size()) {issue("unknown opcode");return issues;}
    const auto& params=definitions[rule.opcode].params;
    for(size_t s=0;s<params.size();++s) {
        const auto& raw=rule.slot[s];const auto value=folded(raw);
        const std::string field="operand "+std::to_string(s+1)+" ("+paramLabel(params[s])+"): ";
        if(raw.size()>63)issue(field+"operand exceeds the CRT 63-byte limit");
        if(raw.find('\0')!=std::string::npos)issue(field+"embedded NUL would truncate the saved value");
        if(params[s]==PKind::Location && !value.empty() && value!="anywhere" &&
           std::none_of(scenario.regions.begin(),scenario.regions.end(),[&](const auto& region){return folded(region.name)==value;}))issue(field+"unknown region: "+raw);
        if(params[s]==PKind::UnitType && !registry.find(value)) {
            const bool wildcard=value=="any unit";
            const bool supported=wildcard && !(action && rule.opcode==7);
            if(!supported)issue(field+(wildcard?"Any Unit is not supported by this rule in the current engine":"unknown unit type: "+raw),!wildcard);
        }
        if(params[s]==PKind::Flag) {
            if(raw.empty())issue(field+"flag name is empty");
            else if(raw.size()>1)issue(field+"retail uses only the first byte of a flag name; names beginning with the same byte share one flag",false);
        }
        if(params[s]==PKind::Player) {
            bool valid=value=="all players";
            for(int p=1;p<=8;++p)valid|=value=="player "+std::to_string(p) || value==std::to_string(p);
            if(!valid)issue(field+"choose All Players or Player 1 through Player 8");
        }
        if(params[s]==PKind::Value) {
            const auto first=raw.find_first_not_of(" \t\r\n"),last=raw.find_last_not_of(" \t\r\n");
            std::string number=first==std::string::npos?"":raw.substr(first,last-first+1);
            if(number.size()>1 && number[0]=='+' && number[1]>='0' && number[1]<='9')number.erase(0,1);
            int parsed=0;const auto result=std::from_chars(number.data(),number.data()+number.size(),parsed);
            if(number.empty() || result.ec!=std::errc{} || result.ptr!=number.data()+number.size())issue(field+"must be a whole number in the signed 32-bit range");
            else if(!action && rule.opcode==23 && (parsed<0 || parsed>100))issue(field+"probability outside 0 through 100 is always false or always true",false);
            else if(!action && (rule.opcode==1 || rule.opcode==2) &&
                    (parsed>std::numeric_limits<int>::max()/30 || parsed<std::numeric_limits<int>::min()/30))issue(field+"game time exceeds the engine tick range");
        }
    }
    return issues;
}
std::vector<MapIssue> validateMap(const tak::tnt::Map& map,
    const tak::tnt::Scenario& metadata,const tak::crt::Scenario& scenario,
    const std::vector<PlacedUnit>& units,const std::set<std::string>& useOnly,
    const tak::sim::TypeRegistry& registry,const tak::hpi::Vfs& vfs) {
    std::vector<MapIssue> issues;
    auto issue=[&](std::string text,float x=-1,float z=-1,bool error=false) {
        issues.push_back({error?MapIssue::Severity::Error:MapIssue::Severity::Warning,std::move(text),x,z});
    };
    if(map.width<=0 || map.height<=0 || map.heights.size()!=size_t(map.width)*map.height ||
       map.features.size()!=map.heights.size()) {
        issue("Invalid terrain dimensions or cell arrays",-1,-1,true);return issues;
    }
    issues=validateTerrainResources(map,vfs);
    auto inside=[&](float x,float z) {return std::isfinite(x) && std::isfinite(z) && x>=0 && z>=0 && x<map.width*16.f && z<map.height*16.f;};
    auto world=std::make_unique<tak::sim::World>();
    world->setTerrain(map.heights,map.width,map.height,map.seaLevel,&map.features);
    world->buildNavClasses(registry);
    tak::sim::registerMapFeatures(*world,map,vfs,&registry);
    std::set<uint16_t> missingFeatures;
    const auto& featureTypes=world->mapPlacementTypes();
    for(size_t i=0;i<map.features.size();++i) {
        const auto feature=map.features[i];if(feature>=0xfffa)continue;
        if((feature>=map.featureNames.size() || feature>=featureTypes.size() || featureTypes[feature].name.empty()) && missingFeatures.insert(feature).second)
            issue("Missing feature definition: "+(feature<map.featureNames.size()?map.featureNames[feature]:std::to_string(feature)),float(i%map.width)*16,float(i/map.width)*16,true);
    }
    std::vector<bool> terrainAccepted;terrainAccepted.reserve(units.size());
    std::set<std::string> restrictions,regions,checkedModels;
    for(const auto& type:useOnly)restrictions.insert(folded(type));
    for(const auto& type:scenario.customTypes)
        if(type.stat[0]!=100)
            issue(type.name+": custom-type health is preserved but retail does not apply it; use placed-unit Health %");
    for(const auto& unit:units) {
        terrainAccepted.push_back(false);
        const auto label=unit.name.empty()?unit.type:unit.name;
        if(unit.name.size()>31)issue(label+": in-game display name is limited to 31 bytes",unit.x,unit.z);
        if(unit.vertical!=200)issue(label+": vertical placement is preserved but ignored by retail",unit.x,unit.z);
        if(!inside(unit.x,unit.z)) {issue(label+": outside map",-1,-1,true);continue;}
        if(unit.player<0 || unit.player>8)issue(label+": invalid owner",unit.x,unit.z,true);
        if(!restrictions.empty() && !restrictions.count(folded(unit.type)))issue(label+": Use Only prevents constructing more of this type (this placement remains)",unit.x,unit.z);
        const auto* type=registry.find(folded(unit.type));
        if(!type) {issue(label+": unknown unit type",unit.x,unit.z,true);continue;}
        if(checkedModels.insert(type->id).second && !vfs.has("objects3d/"+type->id+".3do"))
            issue(label+": unit model is missing (objects3d/"+type->id+".3do)",unit.x,unit.z);
        // Preplaced scenarios may deliberately bypass construction constraints.
        // Report these as warnings rather than forbidding an authored placement.
        terrainAccepted.back()=type->canFly || world->canPlace(type,unit.x,unit.z);
        if(!terrainAccepted.back())issue(label+": engine placement rejects this terrain or feature footprint",unit.x,unit.z);
    }
    if(metadata.starts.empty())issue("No player start positions");
    else if(metadata.starts.size()<2)issue("Fewer than two player starts; ordinary skirmishes need at least two");
    if(metadata.starts.size()>8)issue("More than eight player starts; only eight players can join a match");
    struct StartCandidate {const tak::sim::UnitType* type;float x,z;int number;};
    std::vector<StartCandidate> startCandidates;
    std::set<std::pair<int,int>> startCells;
    std::set<int> startNumbers;
    for(size_t i=0;i<metadata.starts.size();++i) {
        const auto& start=metadata.starts[i];float x=start.xpos*16.f,z=start.zpos*16.f;
        const auto label="Start "+std::to_string(start.number);
        if(start.number<1 || start.number>8)issue(label+": start number must be 1 through 8",x,z,true);
        if(!startNumbers.insert(start.number).second)issue(label+": duplicate start number (the engine keeps only one)",x,z,true);
        if(!inside(x,z)) {issue(label+": outside map",-1,-1,true);continue;}
        if(!startCells.insert({start.xpos,start.zpos}).second)issue(label+": overlaps another start",x,z);
        std::string blocked;
        for(const char* monarch:tak::sim::kMonarchs) {
            const auto* type=registry.find(monarch);
            if(type && !type->canFly) {
                if(!world->canPlace(type,x,z)) {if(!blocked.empty())blocked+=", ";blocked+=type->name;}
                else startCandidates.push_back({type,x,z,start.number});
            }
        }
        if(!blocked.empty())issue(label+": unsuitable for "+blocked,x,z);
    }
    for(int n=1;n<=int(metadata.starts.size()) && n<=8;++n)if(!startNumbers.count(n)) {
        issue("Start numbering has a gap at "+std::to_string(n)+"; use consecutive numbers for predictable slot order");break;
    }
    // Use the same observational connectivity query as the AI. It resolves a
    // blocked endpoint to nearby occupiable ground, so these are approach-route
    // warnings, not guarantees about the exact destination cell or build site.
    if(metadata.starts.size()<=8)for(const char* monarchId:tak::sim::kMonarchs) {
        const auto* monarch=registry.find(monarchId);if(!monarch || monarch->canFly)continue;
        for(size_t i=0;i<metadata.starts.size();++i) {
            const auto& start=metadata.starts[i];const float x=start.xpos*16.f,z=start.zpos*16.f;
            if(!inside(x,z))continue;
            bool connected=false;
            for(size_t j=0;j<metadata.starts.size();++j)if(i!=j) {
                const auto& other=metadata.starts[j];const float ox=other.xpos*16.f,oz=other.zpos*16.f;
                if(inside(ox,oz) && world->pathExists(monarch,ox,oz,x,z)) {connected=true;break;}
            }
            const auto label="Start "+std::to_string(start.number)+" ("+monarch->name+")";
            if(metadata.starts.size()>1 && !connected)issue(label+": no ground approach route to another start; transports may be intentional",x,z);
            if(world->hasManaSpots()) {
                bool reachable=false;
                for(const auto& [mx,mz]:world->manaSpots())if(world->pathExists(monarch,mx,mz,x,z)) {reachable=true;break;}
                if(!reachable)issue(label+": no ground approach route to a mana deposit",x,z);
            }
        }
    }
    if(!world->hasManaSpots() && std::any_of(registry.types().begin(),registry.types().end(),[](const auto& entry){return entry.second.onMana;}))
        issue("No recognized mana deposits; skirmish players cannot expand their lodestone economy");
    struct ManaCandidate {const tak::sim::UnitType* type;float x,z;};
    std::vector<ManaCandidate> manaCandidates;
    for(const auto& [x,z]:world->manaSpots()) {
        std::string blocked;
        for(const auto& [id,type]:registry.types())if(type.onMana &&
            (restrictions.empty() || restrictions.count(folded(id)))) {
            if(world->canPlace(&type,x,z))manaCandidates.push_back({&type,x,z});
            else {if(!blocked.empty())blocked+=", ";blocked+=type.name.empty()?id:type.name;}
        }
        if(!blocked.empty())issue("Mana deposit: engine rejects lodestone footprint for "+blocked,x,z);
    }
    // Classify all distinct mobile movement/footprint profiles, not just routes
    // between starts. Reuse equivalent profiles and release each scan before
    // starting the next so large maps do not retain one label grid per type.
    std::map<std::pair<const tak::sim::NavGrid*,int>,std::vector<std::string>> profiles;
    for(const auto& [id,type]:registry.types()) {
        if(type.canFly || type.maxVel.v<=0 || (!restrictions.empty() && !restrictions.count(folded(id))))continue;
        profiles[{&world->navFor(&type),std::clamp(std::max(type.footX,type.footZ),1,15)}].push_back(type.name.empty()?id:type.name);
    }
    for(const auto& [profile,types]:profiles) {
        const auto regions=movementRegions(*profile.first,profile.second);
        if(regions.size()<2)continue;
        const auto& separate=regions[1];
        std::string label=types.front();if(types.size()>1)label+=" and "+std::to_string(types.size()-1)+" equivalent unit types";
        issue(label+": "+std::to_string(regions.size())+" disconnected movement regions (terrain/features); largest "+
              std::to_string(regions[0].cells)+" cells, next "+std::to_string(separate.cells)+". Islands may be intentional.",separate.x*16.f+8,separate.z*16.f+8);
    }
    // Add occupants only after the terrain/connectivity checks, so diagnostics
    // distinguish authored terrain from units that may move during play.
    world->setPlayerCount(8);
    std::vector<int> factories;
    for(size_t i=0;i<units.size();++i) {
        const auto& unit=units[i];const auto* type=registry.find(folded(unit.type));
        if(!type || !inside(unit.x,unit.z))continue;
        if(!type->canFly && terrainAccepted[i] && !world->canPlace(type,unit.x,unit.z))
            issue((unit.name.empty()?unit.type:unit.name)+": engine placement blocked by earlier preplaced units",unit.x,unit.z);
        // Ownership does not affect this observational occupancy check. Keep
        // the CRT neutral slot valid without inventing an extra match player.
        const int id=world->spawn(type,unit.x,unit.z,unit.angle*3.14159265f/180.f,std::clamp(unit.player,0,7));
        if(type->isStructure() && id)factories.push_back(id);
    }
    for(const int id:factories) {
        const auto* factory=world->unit(id);
        const auto factoryName=factory->type->name;
        const auto factoryType=factory->type->id;
        const float x=factory->x.toFloat(),z=factory->z.toFloat();
        std::string blocked;
        for(const auto& output:registry.buildable(factoryType)) {
            const auto* ship=registry.find(output);
            if(!ship || ship->domain!=tak::sim::UnitType::Domain::Water || ship->isStructure())continue;
            tak::sim::Fixed sx,sy,sz;
            if(!world->productionPosition(id,ship,sx,sy,sz)) {
                if(!blocked.empty())blocked+=", ";
                blocked+=ship->name;
            }
        }
        if(!blocked.empty())issue(factoryName+": initial naval output site cannot place "+blocked+
            ". Check water depth, shoreline and nearby obstacles; this checks the current script pose, not an entire launch animation.",x,z);
    }
    for(const auto& candidate:startCandidates)if(!world->canPlace(candidate.type,candidate.x,candidate.z))
        issue("Start "+std::to_string(candidate.number)+": preplaced units currently block "+candidate.type->name,candidate.x,candidate.z);
    for(const auto& candidate:manaCandidates) {
        const bool hasLodestone=std::any_of(world->units().begin(),world->units().end(),[&](const auto& unit) {
            const float dx=unit.x.toFloat()-candidate.x,dz=unit.z.toFloat()-candidate.z;
            return unit.type && unit.type->onMana && dx*dx+dz*dz<44*44;
        });
        if(!hasLodestone && !world->canPlace(candidate.type,candidate.x,candidate.z))
            issue("Mana deposit: preplaced units currently block "+candidate.type->name,candidate.x,candidate.z);
    }
    for(const auto& region:scenario.regions) {
        if(region.name.empty() || !regions.insert(folded(region.name)).second)issue("Region name is empty or duplicated: "+region.name,-1,-1,true);
        if(std::min(region.x1,region.x2)<0 || std::min(region.z1,region.z2)<0 || std::max(region.x1,region.x2)>=map.width || std::max(region.z1,region.z2)>=map.height)
            issue("Region has out-of-map bounds: "+region.name,-1,-1,true);
    }
    for(size_t p=0;p<scenario.players.size();++p)for(size_t g=0;g<scenario.players[p].size();++g) {
        const auto& group=scenario.players[p][g];
        for(bool action:{false,true}) {
            const auto& rules=action?group.actions:group.conditions;
            for(size_t r=0;r<rules.size();++r) {
                const auto label=(p==0 ? std::string("All Players") : "Player "+std::to_string(p))+", rule "+std::to_string(g+1)+", "+(action?"action ":"condition ")+std::to_string(r+1)+": ";
                for(auto problem:validateRuleOperands(action,rules[r],scenario,registry)) {
                    problem.message=label+problem.message;issues.push_back(std::move(problem));
                }
            }
        }
    }
    return issues;
}
std::vector<MapIssue> validateCrusadesRoster(const tak::crt::Scenario& scenario,
    const std::vector<PlacedUnit>& units,const tak::sim::TypeRegistry& standard,
    const tak::sim::TypeRegistry& crusades) {
    std::vector<MapIssue> issues;
    // The standard unit a name means, when CRUSADES leaves it out.
    auto lost=[&](const std::string& name)->const tak::sim::UnitType* {
        const auto id=folded(name);
        return crusades.find(id)?nullptr:standard.find(id);
    };
    auto label=[](const tak::sim::UnitType& type,const std::string& name) {
        std::string upper=name;
        std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return char(std::toupper(c));});
        return type.name.empty()?upper:type.name+" ("+upper+")";
    };
    auto warn=[&](std::string text,float x=-1,float z=-1) {issues.push_back({MapIssue::Severity::Warning,std::move(text),x,z});};
    // One line per type, at its first placement.
    std::map<std::string,std::pair<size_t,size_t>> placed;   // folded type -> (count, first index)
    for(size_t i=0;i<units.size();++i)if(lost(units[i].type)) {
        ++placed.try_emplace(folded(units[i].type),0,i).first->second.first;
    }
    for(const auto& [id,use]:placed) {
        const auto& unit=units[use.second];
        warn(label(*lost(unit.type),unit.type)+" does not exist with CRUSADES on: "+
             (use.first==1?std::string("this placed unit stops"):std::to_string(use.first)+" placed units stop")+
             " a CRUSADES game from starting this scenario",unit.x,unit.z);
    }
    for(const auto& custom:scenario.customTypes)if(const auto* type=lost(custom.name))
        warn("Custom stats for "+label(*type,custom.name)+" are skipped with CRUSADES on: the unit does not exist there");
    for(size_t p=0;p<scenario.players.size();++p)for(size_t g=0;g<scenario.players[p].size();++g) {
        const auto& group=scenario.players[p][g];
        for(bool action:{false,true}) {
            const auto& rules=action?group.actions:group.conditions;
            const auto& definitions=action?actionDefs():conditionDefs();
            for(size_t r=0;r<rules.size();++r) {
                const auto& rule=rules[r];
                if(rule.opcode<0 || size_t(rule.opcode)>=definitions.size())continue;
                const auto& params=definitions[size_t(rule.opcode)].params;
                for(size_t s=0;s<params.size();++s)if(params[s]==PKind::UnitType)
                    if(const auto* type=lost(rule.slot[s]))
                        // An unknown name resolves to type 0 (retail 515ac0): Create makes
                        // nothing; the other rules match any unit (or, for loss and kill
                        // counts, the first type counted) instead.
                        warn((p==0?std::string("All Players"):"Player "+std::to_string(p))+", rule "+std::to_string(g+1)+", "+
                             (action?"action ":"condition ")+std::to_string(r+1)+": "+label(*type,rule.slot[s])+
                             " does not exist with CRUSADES on, so there this rule "+
                             (action && rule.opcode==7?"creates nothing":"counts other units instead"));
            }
        }
    }
    return issues;
}
std::vector<MapIssue> validateAiProfiles(const AiFiles& files,const std::string& aiProfile,
    const tak::sim::TypeRegistry& registry,const tak::hpi::Vfs& vfs) {
    std::vector<MapIssue> issues;
    const auto catalogue=tak::ai::ClassicCatalogue::from(registry);
    for(const auto& issue:checkAiMap(files,aiProfile,vfs,{&catalogue,&registry})) {
        if(issue.severity==AiIssue::Severity::Info)continue;
        std::string where=issue.file.empty()?"aiprofile":issue.file;
        if(issue.line && issue.message.rfind("line ",0)!=0)where+=" line "+std::to_string(issue.line);
        issues.push_back({issue.severity==AiIssue::Severity::Error?MapIssue::Severity::Error:MapIssue::Severity::Warning,
                          "AI profile "+where+": "+issue.message});
    }
    return issues;
}
}
