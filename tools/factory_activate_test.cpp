// A factory given production on the tick after its build finishes opens and
// produces (0.9.20 owner decision, World::switchScript).
//
// Every factory and yard script's Create sleeps 100 ms at a time until
// BUILD_PERCENT_LEFT reads 0 and only then calls InitState, which sets up the
// RequestState bookkeeping. The AI queued a ship on the tick after its Navy
// Yard finished; the yard's Activate ran while Create was still in that sleep,
// its RequestState(0) found the yard "already on" (the statics still zero), so
// Go and OpenYard never ran and the yard waited for ever (naval_ai's Creon
// opening). An Activate now waits until Create has returned.
//
// For every shipped structure that produces with a script, a builder that has
// it in its menu builds it (land, or the water beside the shore for a yard),
// and the moment it finishes it is given one unit from its own menu. The build
// is repeated at three builder speeds, so the finish lands on each phase of
// Create's 100 ms (three-tick) sleep. Each case must open its yard and finish
// the unit.
#include "cob/cob.h"
#include "hpi/hpi.h"
#include "sim/matchsetup.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace tak;
namespace {
int failures=0,cases=0;
void check(bool ok,const std::string& label) {
    std::printf("[%s] %s\n",ok?"PASS":"FAIL",label.c_str());++cases;if(!ok)++failures;
}
constexpr int kCells=256,kShore=128;   // land west of cell column 128, water east of it

const sim::UnitType* builderOf(const sim::TypeRegistry& registry,const sim::UnitType& factory) {
    for(const auto& [id,t]:registry.types())
        if(t.isBuilder && !t.isStructure() && !t.canFly && t.domain!=sim::UnitType::Domain::Water)
            for(const auto& entry:t.buildMenu)if(entry==factory.id)return &t;
    return nullptr;
}

const sim::UnitType* outputOf(const sim::TypeRegistry& registry,const sim::UnitType& factory) {
    for(const auto& entry:factory.buildMenu)
        if(const auto* t=registry.find(entry);t && !t->isStructure())return t;
    return nullptr;
}

struct Result {int finishTick=-1,opened=-1,produced=-1;};

Result run(const sim::TypeRegistry& registry,const sim::UnitType& factory,const sim::UnitType& builder,
           const sim::UnitType& output,int speed) {
    sim::World w;w.setSerialThreads(true);
    std::vector<uint8_t> heights(kCells*kCells,0);
    for(int z=0;z<kCells;++z)for(int x=0;x<kShore;++x)heights[size_t(z)*kCells+x]=120;
    w.setTerrain(heights,kCells,kCells,60);
    w.buildNavClasses(registry);w.setPathService(true);
    auto fast=builder;fast.workerTime=builder.workerTime*speed;
    // A land factory just inside the shore, else (a yard) just off it; the
    // builder stands on the shore beside the site.
    const float halfX=float(factory.footX*8),z=2048;
    const bool water=!w.canPlace(&factory,float(kShore*16)-halfX-96,z,0);
    const float x=water ? float(kShore*16)+halfX+16 : float(kShore*16)-halfX-96;
    const int b=w.spawn(&fast,water ? float(kShore*16)-24 : x+halfX+40,z,0,0);
    w.player(0).mana=1e7;
    Result r;
    bool blocked=false;
    const int site=w.startBuild(b,&factory,x,z,sim::World::Approach::None,&blocked);
    if(!site) {std::printf("  %s: no site at %.0f,%.0f (mobile blocked %d, placeable %d)\n",factory.id.c_str(),x,z,
                           int(blocked),int(w.canPlace(&factory,x,z,0)));return r;}
    for(int t=0;t<9000;++t) {
        w.player(0).mana=1e7;w.tick(1.f/30);
        const auto* f=w.unit(site);
        if(!f || !f->alive()) {std::printf("  %s: the site is gone on tick %d\n",factory.id.c_str(),t);return r;}
        if(r.finishTick<0 && !f->underConstruction) {r.finishTick=t;w.train(site,&output,1);}
        if(r.finishTick<0)continue;
        if(r.opened<0 && w.scriptYardOpen(site))r.opened=t-r.finishTick;
        for(const auto& u:w.units())
            if(u.alive() && u.type==&output && !u.underConstruction && u.id!=b) {r.produced=t-r.finishTick;return r;}
    }
    return r;
}
}

// cob::createHold on hand-made Creates: a build-wait (sleep while
// BUILD_PERCENT_LEFT) followed by a RETURN holds over the whole of Create; the
// same followed by an endless loop holds only up to the build-wait's exit.
void createHoldShapes() {
    cob::File f;f.scripts={{"Create",0}};
    const uint32_t push=0x10021001,get=0x10042000,jif=0x10066000,sleep=0x10013000,jump=0x10064000,ret=0x10065000;
    f.code={push,17,get,jif,10,push,100,sleep,jump,0,   // 0..9: while(BUILD_PERCENT_LEFT) sleep 100
            push,100,sleep,ret};                        // 10..13
    auto hold=cob::createHold(f);
    check(hold.begin==0 && hold.end==14,"a Create that returns holds until it returns");
    f.code.back()=jump;f.code.push_back(10);            // 13: jump 10, for ever
    hold=cob::createHold(f);
    check(hold.begin==0 && hold.end==10,"an endless Create holds only through its build-wait");
    f.code={push,100,sleep,jump,0};                     // endless, no build-wait
    hold=cob::createHold(f);
    check(hold.begin==hold.end,"an endless Create with no build-wait holds nothing");
}

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3)return 2;
    const std::string only=argc==3 ? argv[2] : "";
    createHoldShapes();
    auto vfs=hpi::mountRetailRoot(argv[1]);
    sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
    int factories=0;
    for(const auto& [id,factory]:registry.types()) {
        if(!factory.producesUnits() || !factory.productionScript)continue;
        if(!only.empty() && id!=only)continue;
        const auto* builder=builderOf(registry,factory);
        const auto* output=outputOf(registry,factory);
        if(!builder || !output) {std::printf("skip %s: no builder or output\n",id.c_str());continue;}
        ++factories;
        for(int speed:{20,21,22}) {
            const Result r=run(registry,factory,*builder,*output,speed);
            std::printf("%s by %s x%d -> %s: finished %d, opened %+d, produced %+d\n",id.c_str(),builder->id.c_str(),
                        speed,output->id.c_str(),r.finishTick,r.opened,r.produced);
            check(r.finishTick>=0 && r.opened>=0 && r.produced>=0,
                  id+" (builder x"+std::to_string(speed)+") given "+output->id+" as it finishes opens and produces it");
        }
    }
    check(only.size() || factories>=10,"every shipped production structure was tried ("+std::to_string(factories)+")");
    std::printf("factory activate: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
