#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include "sim/footprint.h"
#include "sim/retailinstantcorpse.h"
#include "cob/vm.h"
#include <cstdio>
using namespace ta;
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    int failures=0;
    auto check=[&](bool ok,const char* message) {
        if (!ok) {++failures;std::fprintf(stderr,"FAIL: %s\n",message);}
    };
    {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs);
        sim::World definitions;
        sim::registerMapFeatures(definitions,tnt::Map{},vfs,&registry);
        // TA's Killed scripts pick the corpse from the severity; none is the
        // constant handler the Kingdoms instant-ruin/explosion-corpse analyzers
        // (retailinstantcorpse.h) prove, so the cases below take shipped units
        // whose Killed, run independently, leaves wreck 1 at severity 20.
        for (const auto& [name,type]:registry.types())
            check(!type.instantCorpse && !type.explosionCorpse,"no TA script is a constant Kingdoms corpse handler");
        const std::vector<const char*> wrecked{"armsolar","armllt","armpw","armsub","armpt","armroy"};
        int restored=0,waterStays=0,waterFades=0;uint64_t hash=0;
        for (const char* name:wrecked) {
            const auto* found=registry.find(name);
            check(found && found->script(),"shipped wreck unit loads");if (!found || !found->script()) continue;
            const auto& type=*found;
            ++restored;
            cob::Vm vm(*type.script());vm.enableRetailAnimation();
            vm.call("Killed",{20,0,3});
            check(vm.lastLocals().size()>1 && (vm.lastLocals()[1]&15)==1,
                  "Killed at severity 20 requests wreck 1");
            const int ct=definitions.corpseTypeOf(&type);
            check(ct>=0,"restored corpse definition exists");if (ct<0) continue;
            for (bool water:{false,true}) {
                sim::World w;
                w.setTerrain(std::vector<uint8_t>(64*64,water?0:100),64,64,20);
                w.setFeatureTypes(definitions.featureTypes());w.mapCorpse(&type,ct);
                const int id=w.spawn(&type,512,512,std::nullopt,0);
                w.unit(id)->hp={};w.unit(id)->deathType=3;
                w.tick(1.f/30);
                check(w.unit(id)->deathHasCorpse,
                      "explosion preserves the authored corpse on land and water");
                for(int i=0;i<1500 && w.unit(id)->deadFor<w.unit(id)->corpseAnimationTicks();++i)
                    w.tick(1.f/30);
                check(w.unit(id)->deadFor==w.unit(id)->corpseAnimationTicks() &&
                      w.unit(id)->corpseUntil>w.unit(id)->deadFor,
                      "restored corpse survives its authored death-animation handoff");
                if(water) {
                    // A sea bed of 0: a body at its own height (a wreck on
                    // the bed) lands at once (y <= 511260's 0) and lies there
                    // until it decomposes as a land corpse would (not retail,
                    // whose water grave has no decomposition); one held above
                    // the bed (a ship at its waterline) fades out as it
                    // reaches it and leaves the plane.
                    const auto* first=w.corpseSink(id);
                    check(first!=nullptr,"a water grave sinks");
                    const bool above=first && (first->fading || first->y>0);
                    bool gone=false;
                    for(int i=0;i<600 && !gone;++i) {
                        w.tick(1.f/30);
                        gone=!w.unit(id) || w.unit(id)->corpseUntil==0;
                    }
                    const auto* sink=w.corpseSink(id);
                    check(above ? gone && !sink : gone || (sink && sink->y==0),
                          "a water grave lies on a bed of 0 until it decomposes, or fades out reaching it");
                    (above ? waterFades : waterStays)++;
                }
                hash=(hash*1099511628211ULL)^w.stateHash();
            }
            std::printf("WRECK %s\n",name);
        }
        // 512ee0 (512f8f..512fab, emulated over 3000 layouts): the corpse is a
        // water grave when 511170's interpolated ground height under the body
        // is at or below sea level, whatever its cell's corner says.
        {
            const auto& type=*registry.find("armsolar");
            const int ct=definitions.corpseTypeOf(&type);
            int decomposeOverride=-1;
            // The corpse's lifetime after its placement and whether it sinks.
            auto grave=[&](const std::vector<uint8_t>& heights,int32_t x,int32_t z,int32_t y=-1) {
                sim::World w;w.setTerrain(heights,64,64,20);
                auto features=definitions.featureTypes();
                if (decomposeOverride>=0) features[size_t(ct)].decomposeTicks=decomposeOverride;
                w.setFeatureTypes(features);w.mapCorpse(&type,ct);
                const int id=w.spawn(&type,512,512,std::nullopt,0);
                w.unit(id)->x=sim::Fixed::raw(x);w.unit(id)->z=sim::Fixed::raw(z);
                if (y>=0) w.unit(id)->groundY=sim::Fixed::raw(y);
                w.unit(id)->hp={};w.unit(id)->deathType=3;
                w.tick(1.f/30);
                for(int i=0;i<1500 && w.unit(id)->deadFor<w.unit(id)->corpseAnimationTicks();++i)
                    w.tick(1.f/30);
                return std::pair{int64_t(w.unit(id)->corpseUntil)-w.unit(id)->deadFor,w.corpseSink(id)!=nullptr};
            };
            auto lifetime=[&](const std::vector<uint8_t>& heights,int32_t x,int32_t z,int32_t y=-1) {
                return grave(heights,x,z,y).first;
            };
            const int decompose=definitions.featureTypes()[size_t(ct)].decomposeTicks;
            const int64_t land=lifetime(std::vector<uint8_t>(64*64,100),512<<16,512<<16);
            // 4959c0 (E, check_corpse_decay.py): the decomposition, then 60
            // updates sinking into the ground. Before: the decomposition alone.
            check(decompose>0 ? land==decompose+58 : land>=INT32_MAX-3000,
                  "a wreck on land decomposes, then sinks into the ground");
            decomposeOverride=90;
            check(lifetime(std::vector<uint8_t>(64*64,100),512<<16,512<<16)==90+58,
                  "a wreck with a decomposition of 90 ticks goes 148 ticks after it is laid");
            check(lifetime(std::vector<uint8_t>(64*64,5),512<<16,512<<16,18<<16)==90+58,
                  "a water grave decomposes as a wreck on land does");
            decomposeOverride=-1;
            const auto level=grave(std::vector<uint8_t>(64*64,20),512<<16,512<<16);
            check(level.second && level.first==land,
                  "a body on ground level with the sea is a water grave, with a land corpse's lifetime");
            std::vector<uint8_t> corner(64*64,100);corner[32*64+32]=0;
            const auto dry=grave(corner,(32*16+15)<<16,(32*16+15)<<16);
            check(!dry.second && dry.first==land,
                  "a body on dry ground over a sunken cell corner became a water grave");
        }
        // The sinking (4959c0, emulated: tools/re/check_corpse_sink.py and
        // check_water_grave.py): a grave sinks 0x2ccc a tick from the body's height to 511260's sea
        // bed and lies there; over a bed of 0 it fades as it falls and leaves
        // the plane. Each corpse is placed at the body's height at sea level
        // (20), over a bed of 0 or 10.
        {
            const auto* keep=registry.find("armsolar");
            auto sinking=[&](const sim::UnitType* type,int bed,int& gone,int32_t& rest) {
                sim::World w;w.setTerrain(std::vector<uint8_t>(64*64,uint8_t(bed)),64,64,20);
                w.setFeatureTypes(definitions.featureTypes());
                w.mapCorpse(type,definitions.corpseTypeOf(type));
                const int id=w.spawn(type,512,512,std::nullopt,0);
                w.unit(id)->groundY=sim::Fixed::fromInt(20);
                w.unit(id)->hp={};w.unit(id)->deathType=1;
                gone=-1;rest=-1;
                for (int t=0;t<1200 && gone<0;++t) {
                    w.tick(1.f/30);
                    const auto* c=w.unit(id);
                    if (c->corpseUntil==0) gone=t;
                    else if (const auto* sink=w.corpseSink(id);sink && !sink->vy) rest=sink->y;
                }
            };
            int gone;int32_t rest;
            // 1310720 - 11468 = 1299252 after the placing tick's pass; the
            // fade's step is 11468 / 1299252, so the 114th pass after it ends
            // the alpha (113.3 steps of fall) and the wreck leaves the plane.
            sinking(keep,0,gone,rest);
            check(gone==114,"a wreck over a sea bed of 0 fades out as it reaches the bed (tick 114)");
            sinking(keep,10,gone,rest);
            check(gone<0 && rest==(10<<16),"a wreck over a sea bed of 10 lies on it for good");
        }
        check(restored==int(wrecked.size()),"every shipped wreck case ran");
        check(waterStays>0 && waterFades>0,"both a wreck at rest on a bed of 0 and a fading one were seen");
        for(bool unfinished:{false,true}) {
            sim::World w;
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            const auto* type=registry.find("armsolar");
            w.setFeatureTypes(definitions.featureTypes());w.mapCorpse(type,definitions.corpseTypeOf(type));
            const int id=w.spawn(type,512,512,std::nullopt,0);
            sim::UnitType builderType;builderType.maxHp=100;
            if(unfinished) {
                const int builder=w.spawn(&builderType,32,32,std::nullopt,0);
                w.unit(builder)->buildSiteId=id;
            }
            auto* unit=w.unit(id);unit->underConstruction=unfinished;unit->buildBegun=unfinished;
            unit->hp={};unit->deathType=unfinished?3:sim::Unit::kDeathSelfDestruct;
            w.tick(1.f/30);
            check(w.unit(id)->deadFor==0,"actual death rather than abandoned-site removal");
            // An unfinished unit leaves nothing (0x4865d2..0x4865e5). A
            // self-destruct is no exception in TotalA.exe: its Killed runs and
            // names the corpse (0x4865c3) -- a 1 here, at a severity of 1.
            if(unfinished)check(w.unit(id)->corpseUntil<=w.unit(id)->corpseAnimationTicks(),
                  "unfinished units remain excluded");
            else check(w.unit(id)->severity>0 && w.unit(id)->deathHasCorpse,
                  "a self-destructed unit runs Killed and keeps its corpse");
        }
        std::printf("wrecks hash=%016llx\n",(unsigned long long)hash);
    }
    return failures?1:0;
}
