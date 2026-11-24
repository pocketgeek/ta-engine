// Exercise shipped movement/footprints/scripts: synthetic units with no retail
// goal-crowding tolerance did not expose output parking over the next birthplace.
//
// Large outputs (first reported with a Kingdoms catapult that "got stuck in the
// entrance and I had to kill it so other stuff could build"): every shipped factory
// makes its largest ground unit and three of its smallest, the large one first or
// last. The next site used to be admitted by a centre-radius test sized for the NEW
// unit only, so a small unit was born behind a departing large one with their
// footprints overlapping. Movement commits are footprint-strict on a loaded map, so
// neither could ever take a step again. Retail's BUILD stage 2 (401deb) admits a
// site with the footprint-strict mobile test (507d10, no moving-body exemption) and
// otherwise waits; every pair of outputs must therefore stay apart, the large unit
// must clear the factory, all four finish and none is left standing in the yard.
#include "sim/matchsetup.h"
#include "hpi/hpi.h"
#include <cmath>
#include <cstdio>
#include <array>
#include <algorithm>
#include <string>
#include <vector>
namespace {
struct Rect {int x,z,w,h;};
Rect footprint(const ta::sim::Unit& u) {
    return {ta::sim::footprintOrigin(u.x,u.type->footX),ta::sim::footprintOrigin(u.z,u.type->footZ),u.type->footX,u.type->footZ};
}
bool intersects(const Rect& a,const Rect& b) {return a.x<b.x+b.w && b.x<a.x+a.w && a.z<b.z+b.h && b.z<a.z+a.h;}
bool groundOutput(const ta::sim::UnitType* t) {
    return t && !t->isStructure() && !t->canFly && t->domain==ta::sim::UnitType::Domain::Ground;
}
// One factory at one birth heading (`side` -1/0/+1: either end or the middle of
// its buildangle range, as spawn draws it), on its cell grid as a placed building
// stands: `large` once and three `small`, the large one first or last. Returns
// false on failure.
bool largeOutputCase(const ta::sim::TypeRegistry& registry,const ta::sim::UnitType& factory,
                     const ta::sim::UnitType* large,const ta::sim::UnitType* small,
                     int side,bool legion,bool largeFirst) {
    auto producer=factory;producer.storage=1000000;
    ta::sim::World world;
    if(legion)world.setPathfindingMode(ta::sim::PathfindingMode::Legion);
    // A loaded map's feature plane: movement and placement then take the
    // native rectangular footprint test, as in a real game.
    world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const int half=factory.buildAngle/2;
    const auto heading=uint16_t(factory.orientation+(side<0 ? -half : side>0 ? std::max(half-1,0) : 0));
    const int id=world.spawn(&producer,float(112*16+factory.footX%2*8),float(112*16+factory.footZ%2*8),
        float(int16_t(heading))*6.28318530718f/65536.f,0);
    world.player(0).mana=1000000;
    if(largeFirst)world.train(id,large,1);
    world.train(id,small,3);
    if(!largeFirst)world.train(id,large,1);
    const Rect yard=footprint(*world.unit(id));
    int complete=0,tick=0,overlapTicks=0,largeId=0,largeDone=-1,largeClear=-1;
    std::string firstOverlap;
    for(;tick<16000 && (complete<4 || largeClear<0);++tick) {
        world.tick(1.f/30);complete=0;
        std::vector<const ta::sim::Unit*> outputs;
        for(const auto& u:world.units()) {
            if(u.id==id || !u.alive() || !u.type)continue;
            outputs.push_back(&u);
            if(!largeId && u.type==large)largeId=u.id;
            if(!u.underConstruction)++complete;
        }
        if(const auto* l=largeId ? world.unit(largeId) : nullptr; l && l->alive() && !l->underConstruction) {
            if(largeDone<0)largeDone=tick;
            if(largeClear<0 && !intersects(footprint(*l),yard))largeClear=tick;
        }
        bool overlapped=false;
        for(size_t i=0;i<outputs.size();++i)for(size_t j=i+1;j<outputs.size();++j)
            if(intersects(footprint(*outputs[i]),footprint(*outputs[j]))) {
                if(firstOverlap.empty())firstOverlap="tick "+std::to_string(tick)+": "+outputs[i]->type->id+
                    (outputs[i]->underConstruction?"(site)":"")+" x "+outputs[j]->type->id+(outputs[j]->underConstruction?"(site)":"");
                overlapped=true;
            }
        overlapTicks+=overlapped;
    }
    // Once everything has been made and has settled, nothing may still stand in
    // the factory's rectangle (its doorway rows included).
    for(int settle=0;settle<900;++settle)world.tick(1.f/30);
    int inYard=0;
    for(const auto& u:world.units())
        if(u.id!=id && u.alive() && u.type && intersects(footprint(u),yard))++inYard;
    const bool pass=complete>=4 && largeClear>=0 && overlapTicks==0 && inYard==0;
    std::printf("%s: large mode=%s factory=%s large=%s small=%s order=%s heading=%d completed=%d "
                "largeClearAfter=%d overlapTicks=%d inYard=%d ticks=%d hash=%016llx%s%s\n",
        pass?"PASS":"FAIL",legion?"Legion":"Retail",factory.id.c_str(),
        large->id.c_str(),small->id.c_str(),largeFirst?"large-first":"large-last",int(heading),complete,largeClear<0 ? -1 : largeClear-largeDone,overlapTicks,inYard,tick,
        (unsigned long long)world.stateHash(),firstOverlap.empty()?"":" first overlap ",firstOverlap.c_str());
    return pass;
}
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);int failures=0,cases=0;
    {
        ta::sim::TypeRegistry registry;ta::sim::setupRegistry(registry,vfs);
        struct Case {const char* producer;const char* output;bool infinite;int count;};
        // A Kbot Lab's Peewees; a Vehicle Plant on repeat (TA's factory
        // "repeat build"); a Shipyard's first ship; a Core Kbot Lab on repeat
        // with its first ground output.
        std::array<Case,4> roster{{{"armlab","armpw",false,24},
            {"armvp","armflash",true,8},{"armsy",nullptr,false,6},{"corlab",nullptr,true,8}}};
        // Each factory stands at its retail birth heading (spawn's
        // orientation +- buildangle/2, 511ba2): TA places no structure turned
        // a quarter round, so the Kingdoms version's four 90-degree turns are
        // gone.
        for(const auto& c:roster) {
            auto producer=*registry.find(c.producer);producer.storage=1000000;
            const auto* output=c.output ? registry.find(c.output) : nullptr;
            if(!c.output)for(const auto& id:registry.buildable(c.producer)) {
                const auto* t=registry.find(id);
                if(t && !t->isStructure() && t->domain==(std::string(c.producer)=="armsy" ? ta::sim::UnitType::Domain::Water : ta::sim::UnitType::Domain::Ground)) {output=t;break;}
            }
            if(!output){std::puts("FAIL: production fixture output missing");++failures;continue;}
            const auto& menu=registry.buildable(c.producer);
            if(std::none_of(menu.begin(),menu.end(),[&](const auto& name){return registry.find(name)==output;})) {
                std::printf("FAIL: %s cannot produce %s in this balance\n",c.producer,output->id.c_str());++failures;continue;
            }
            ta::sim::World world;
            const bool water=output->domain==ta::sim::UnitType::Domain::Water;
            world.setTerrain(std::vector<uint8_t>(256*256,water?0:100),256,256,water?100:20);
            world.buildNavClasses(registry);world.setPathService(true);
            const int id=world.spawn(&producer,1800,1800,std::nullopt,0);
            world.player(0).mana=1000000;
            if(c.infinite)world.setRepeat(id,output);else world.train(id,output,c.count);
            int complete=0,tick=0;
            for(;tick<24000 && complete<c.count;++tick) {
                world.tick(1.f/30);complete=0;
                for(const auto& u:world.units())if(u.id!=id && u.alive() && !u.underConstruction)++complete;
            }
            const bool pass=complete>=c.count && world.unit(id)->rally.empty() &&
                (!c.infinite || world.unit(id)->repeatType==output);
            std::printf("%s: producer=%s output=%s repeat=%d completed=%d ticks=%d hash=%016llx\n",
                pass?"PASS":"FAIL",c.producer,output->id.c_str(),
                c.infinite,complete,tick,(unsigned long long)world.stateHash());
            ++cases;if(!pass)++failures;
        }
        // Every shipped ground factory: its largest ground output and its smallest.
        // One pair (a Vehicle Plant's Construction Vehicle, then Flashes) is
        // pinned explicitly.
        int factories=0;
        for(const auto& [name,type]:registry.types()) {
            if(!type.producesUnits() || !type.productionScript)continue;
            const ta::sim::UnitType *large=nullptr,*small=nullptr;
            for(const auto& id:registry.buildable(name)) {
                const auto* t=registry.find(id);
                if(!groundOutput(t))continue;
                if(!large || t->footX*t->footZ>large->footX*large->footZ)large=t;
                if(!small || t->footX*t->footZ<small->footX*small->footZ)small=t;
            }
            if(!large || large->footX*large->footZ==small->footX*small->footZ)continue;
            ++factories;std::printf("ground factory %s: %s %dx%d, %s %dx%d\n",name.c_str(),large->id.c_str(),large->footX,large->footZ,small->id.c_str(),small->footX,small->footZ);
            for(int side:{-1,0,1})for(bool legion:{false,true})for(bool largeFirst:{true,false}) {
                ++cases;if(!largeOutputCase(registry,type,large,small,side,legion,largeFirst))++failures;
            }
        }
        // TA ships four factories whose ground outputs come in more than one footprint size
        // (armvp, armavp, corvp, coralab); Kingdoms had eight.
        if(factories<4){std::printf("FAIL: only %d shipped ground factories found\n",factories);++failures;}
        {
            const auto check=[&](bool ok,const char* what){std::printf("%s: %s\n",ok?"PASS":"FAIL",what);++cases;if(!ok)++failures;};
            // A factory's output stance (engine addition): none at first, so the
            // output keeps its own default; set, every new unit takes it; the lit
            // one clicked again (kStanceNone) clears it. A computer's factory
            // never takes one from its own stance orders.
            const auto* lab=registry.find("armlab");const auto* peewee=registry.find("armpw");
            auto made=[&](ta::sim::World& world,int factory) {
                const size_t before=world.units().size();
                world.train(factory,peewee,1);
                for(int tick=0;tick<9000;++tick) {
                    world.tick(1.f/30);
                    for(size_t i=before;i<world.units().size();++i) {
                        const auto& u=world.units()[i];
                        if(u.alive() && !u.underConstruction && u.type==peewee) return u.stance;
                    }
                }
                return -9;
            };
            {
                ta::sim::World world;world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
                world.buildNavClasses(registry);world.player(0).mana=1000000;
                const int factory=world.spawn(lab,1800,1800,0,0);
                const int base=world.spawn(peewee,600,600,0,0);
                const int fallback=world.unit(base)->stance;
                check(world.unit(factory)->outputStance==-1 && made(world,factory)==fallback,
                      "a factory with no output stance makes units with their own default");
                world.setStance(factory,2);
                check(world.unit(factory)->outputStance==2 && made(world,factory)==2,"a factory's output stance goes to its new units");
                world.setStance(factory,ta::sim::World::kStanceNone);
                check(world.unit(factory)->outputStance==-1 && made(world,factory)==fallback,
                      "clearing the output stance gives new units their own default again");
                world.player(0).automaticGates=true;world.setStance(factory,0);
                check(world.unit(factory)->outputStance==-1,"a computer's factory takes no output stance");
            }
        }
        const auto *plant=registry.find("armvp"),*builder=registry.find("armcv"),*flash=registry.find("armflash");
        if(!plant || !builder || !flash){std::puts("FAIL: Vehicle Plant/Construction Vehicle/Flash missing");++failures;}
        else for(int side:{-1,0,1})for(bool legion:{false,true}) {
            ++cases;if(!largeOutputCase(registry,*plant,builder,flash,side,legion,true))++failures;
        }
    }
    std::printf("production exits: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
