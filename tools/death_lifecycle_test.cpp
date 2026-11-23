#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/footprint.h"
#include "cob/vm.h"
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>
using namespace tak;

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const auto vfs=hpi::mountRetailRoot(argv[1]);
    int failures=0, cases=0;
    for(bool balance:{false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,balance);
        sim::World definitions;sim::registerMapFeatures(definitions,tnt::Map{},vfs,&registry);
        auto run=[&](const sim::UnitType& type,int death,bool warm,bool unfinished,
                     int expectedHandoff=-1,bool checkQuery=true) {
            sim::World w;
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const int ct=definitions.corpseTypeOf(&type);
            w.setFeatureTypes(definitions.featureTypes());if(ct>=0)w.mapCorpse(&type,ct);
            const int id=w.spawn(&type,512,512,std::nullopt,0);
            sim::UnitType assignedBuilder;
            if(unfinished) {
                // An assigned, distant builder prevents abandonment decay from
                // retiring the site before the actual death dispatcher runs.
                assignedBuilder.id="death-test-builder";assignedBuilder.maxHp=100;
                const int builder=w.spawn(&assignedBuilder,32,32,std::nullopt,0);
                w.unit(builder)->buildSiteId=id;
            }
            if(warm)w.tick(1.f/30);
            auto* u=w.unit(id);u->underConstruction=unfinished;u->buildBegun=unfinished;
            u->hp={};u->deathType=uint8_t(death);
            if(unfinished) {
                for(const auto& candidate:w.units())
                    if(candidate.type==&assignedBuilder)w.unit(candidate.id)->buildSiteId=id;
            }
            w.tick(1.f/30);u=w.unit(id);
            auto check=[&](bool ok,const char* detail) {
                if(!ok){++failures;std::fprintf(stderr,"FAIL %s balance=%d death=%d warm=%d unfinished=%d: %s (handoff=%d corpse=%d)\n",
                    type.id.c_str(),balance,death,warm,unfinished,detail,u->corpseAnimationTicks(),u->deathHasCorpse);}
            };
            check(!u->alive() && u->deadFor==0,"actual death dispatcher ran (not abandoned construction)");
            bool expectedCorpse=false;
            if(!unfinished && death!=sim::Unit::kDeathSelfDestruct && type.script()) {
                cob::Vm vm(*type.script());vm.enableRetailAnimation();
                vm.onGet=[](int32_t value,const std::vector<int32_t>&){return value==18?1:0;};
                vm.call("Killed",{u->severity,0,death});
                expectedCorpse=ct>=0 && vm.lastLocals().size()>1 && (vm.lastLocals()[1]&15)==1;
            }
            if(checkQuery)check(u->deathHasCorpse==expectedCorpse,"corpse agrees with independent Killed output");
            const bool checkBlocking=type.id=="araat" && death==1 && !unfinished && ct>=0
                && w.featureTypes()[size_t(ct)].blocking;
            const int cx=sim::footprintOrigin(u->x,type.footX)+type.corpseAdjX;
            const int cz=sim::footprintOrigin(u->z,type.footZ)+type.corpseAdjZ;
            if(checkBlocking)check(w.nav().walkable(cx,cz),"ruin does not block before authored handoff");
            for(int elapsed=0;elapsed<1500 && u->deadFor<u->corpseAnimationTicks();++elapsed) {
                w.tick(1.f/30);u=w.unit(id);
                if(checkBlocking && u->deadFor<u->corpseAnimationTicks())
                    check(w.nav().walkable(cx,cz),"ruin waits for owner retirement before blocking");
            }
            if(checkBlocking)check(!w.nav().walkable(cx,cz),"ruin blocks on authored handoff");
            check(u->corpseAnimationTicks()!=std::numeric_limits<int32_t>::max(),"authored death retires within bounded trace");
            if(expectedHandoff>=0)check(u->corpseAnimationTicks()==expectedHandoff,"native retirement tick matches");
            if(!u->deathHasCorpse)check(u->corpseUntil<=u->corpseAnimationTicks(),"refused corpse never gains a reclaim window");
            std::printf("DEATH %s balance=%d death=%d warm=%d unfinished=%d corpse=%d handoff=%d\n",
                        type.id.c_str(),balance,death,warm,unfinished,u->deathHasCorpse,u->corpseAnimationTicks());
            ++cases;
        };
        for(const auto& [name,type]:registry.types()) {
            if(!type.script())continue;
            for(int death:{1,3})run(type,death,false,false);
        }
        for(const auto* name:{"arafly","aragren","crefire","npcflag","npcthesh","zonamoe","zonbasil"}) {
            const auto* type=registry.find(name);
            if(!type){++failures;continue;}
            const int ticks=(std::string_view(name)=="zonamoe" || std::string_view(name)=="zonbasil")?67:
                (std::string_view(name)=="npcflag" || std::string_view(name)=="npcthesh")?35:1;
            run(*type,1,true,false,ticks);
        }
        // Values independently observed in native 512610/512860/51d3e0.
        for(auto [name,ticks]:std::array<std::pair<const char*,int>,4>{{
            {"araat",27},{"araarch",28},{"crebomb",147},{"arakeep",0}}}) {
            const auto* type=registry.find(name);if(!type){++failures;continue;}
            run(*type,1,true,false,ticks);
            run(*type,sim::Unit::kDeathSelfDestruct,true,false,ticks);
            run(*type,1,true,true,-1);
        }
    }
    std::printf("death lifecycle: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
