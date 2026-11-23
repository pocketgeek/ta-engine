// In-place lodestone replacement through the real placement/construction path.
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <cstdio>
#include <memory>
using namespace tak;
static int failures=0;
static void check(bool ok,const char* message) {
    if (!ok) {++failures;std::fprintf(stderr,"FAIL: %s\n",message);}
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    auto vfs=hpi::mountRetailRoot(argv[1]);
    for (bool crusades:{false,true}) {
        sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,crusades);
        for (const char* race:{"ara","tar","ver","zon","cre"}) {
            const auto* base=registry.find(std::string(race)+"lode");
            const auto* advanced=registry.find(std::string(race)+"mana");
            const sim::UnitType* builder=nullptr;
            for (const auto& [id,type]:registry.types()) {
                const auto& menu=registry.buildable(id);
                if (type.isBuilder && !type.isStructure() &&
                    std::find(menu.begin(),menu.end(),advanced->id)!=menu.end()) {builder=&type;break;}
            }
            check(base && advanced && builder,"faction has base, advanced and an authorized mobile builder");
            if (!builder) continue;
            auto world=std::make_unique<sim::World>();auto& w=*world;
            w.setPlayerCount(2);
            w.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
            w.setManaSpots({{512,512}});
            w.setSacredSites({{31,31,2,2,1.0f}});
            const int bid=w.spawn(builder,512,608,std::nullopt,0);
            // Give the fixture enough storage/income without changing the real builder's work rate.
            sim::UnitType bank;bank.id="bank";bank.maxHp=100;bank.income=1000;bank.storage=100000;
            w.spawn(&bank,128,128,std::nullopt,0);
            const int old=w.startBuild(bid,base,512,512,sim::World::Approach::None);
            check(old!=0,"base lodestone admitted");if (!old) continue;
            w.unit(old)->underConstruction=false;w.unit(old)->hp=sim::Fixed::fromInt(base->maxHp);
            w.cancelBuilds(bid);w.stop(bid);
            w.player(0).mana=50000;
            check(!w.canPlace(base,512,512,0),"cannot stack basic lodestones");
            check(!w.canPlace(advanced,504,504),"ownerless placement cannot replace buildings");
            check(!w.canPlace(advanced,504,504,1),"cannot upgrade another player's lodestone");
            for (const char* other:{"ara","tar","ver","zon","cre"})
                if (std::string(other)!=race)
                    check(!w.canPlace(registry.find(std::string(other)+"mana"),504,504,0),
                          "cannot replace a different faction's base");
            w.unit(old)->underConstruction=true;
            check(!w.canPlace(advanced,504,504,0),"unfinished basic lodestone cannot be consumed");
            w.unit(old)->underConstruction=false;
            check(w.canPlace(advanced,504,504,0),"own base admits larger advanced footprint");
            const int blocker=w.spawn(registry.find("arasword"),488,488,std::nullopt,1);
            check(!w.canPlace(advanced,504,504,0),"other bodies still block the enlarged footprint");
            w.unit(blocker)->deadFor=sim::World::kRetiredTicks;
            w.setUnitCap(w.player(0).unitCount);
            w.queueBuild(bid,advanced,504,504,false);
            int site=0;
            for (int tick=0;tick<900 && !site;++tick) {
                w.tick(1.0f/30.0f);site=w.unit(bid)->buildSiteId;
            }
            check(site!=0,"queued upgrade approaches and starts at unit cap");if (!site) continue;
            check(!w.unit(old)->alive() && w.unit(site)->lodestoneReplacement &&
                  w.unit(site)->lodestoneReplacement->type==base,"old building consumed and its presentation retained");
            check(!w.canPlace(advanced,504,504,0),"second upgrade cannot claim an active site");
            const double paidBefore=w.unit(site)->lodestoneReplacement->completedWork;
            sim::UnitType starvingBuilder=*builder;starvingBuilder.income=0;
            w.unit(bid)->type=&starvingBuilder;bank.income=0;w.player(0).mana=0;
            for (int tick=0;tick<30;++tick) w.tick(1.0f/30.0f);
            check(w.unit(site)->lodestoneReplacement->completedWork==paidBefore,
                  "mana starvation pauses the upgrade even when healing changes HP");
            w.unit(bid)->type=builder;bank.income=1000;w.player(0).mana=50000;
            const int expected=int(advanced->buildTime/builder->workerTime*30);
            int ticks=0;bool falling=false,rising=false;
            for (;ticks<expected+300 && w.unit(site)->underConstruction;++ticks) {
                w.tick(1.0f/30.0f);
                const auto* u=w.unit(site);
                falling|=u->constructionEmissions[0]>0;
                rising|=u->constructionEmissions[1]>0;
                if (u->underConstruction) check(w.player(0).income==bank.income+builder->income,"upgrade has no double income");
            }
            check(!w.unit(site)->underConstruction,"upgrade completes");
            check(ticks>=expected-20 && ticks<=expected+20,"transition fits normal advanced build duration");
            check(falling && rising,"both unconjure and conjure sparkle directions emitted");
            check(w.player(0).unitCount==3,"upgrade replaces rather than adds a live unit");
            w.tick(1.0f/30.0f);
            check(w.player(0).income>bank.income+builder->income,"completed upgrade produces mana");
            std::printf("%s balance=%d builder=%s ticks=%d expected=%d hash=%016llx\n",race,crusades,builder->id.c_str(),ticks,expected,(unsigned long long)w.stateHash());
            if (std::string(race)=="ara") for (bool destroy:{false,true}) {
                sim::World canceled;
                canceled.setTerrain(std::vector<uint8_t>(64*64,100),64,64,20);
                canceled.setManaSpots({{512,512}});
                const int worker=canceled.spawn(builder,512,608,std::nullopt,0);
                canceled.spawn(&bank,128,128,std::nullopt,0);
                const int original=canceled.startBuild(worker,base,512,512,sim::World::Approach::None);
                canceled.unit(original)->underConstruction=false;
                canceled.unit(original)->hp=sim::Fixed::fromInt(base->maxHp);
                canceled.cancelBuilds(worker);canceled.stop(worker);
                canceled.queueBuild(worker,advanced,504,504,false);
                int upgrade=0;
                for (int tick=0;tick<900 && !upgrade;++tick) {
                    canceled.tick(1.0f/30.0f);upgrade=canceled.unit(worker)->buildSiteId;
                }
                check(upgrade!=0,"cancellation fixture starts queued upgrade");
                if (!upgrade) continue;
                for (int tick=0;tick<120;++tick) canceled.tick(1.0f/30.0f);
                canceled.cancelBuilds(worker);canceled.stop(worker);
                if (destroy) canceled.unit(upgrade)->hp=sim::Fixed();
                for (int tick=0;tick<expected && canceled.unit(upgrade) && canceled.unit(upgrade)->alive();++tick)
                    canceled.tick(1.0f/30.0f);
                check(!canceled.unit(upgrade) || !canceled.unit(upgrade)->alive(),"stopped/destroyed upgrade retires normally");
                check(!canceled.unit(original) || !canceled.unit(original)->alive(),"consumed base does not resume production");
                check(canceled.canPlace(base,512,512,0),"cancellation/destruction releases the mana spot");
            }
        }
    }
    return failures ? 1 : 0;
}
