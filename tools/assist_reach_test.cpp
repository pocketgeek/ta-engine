// assist_reach_test -- a ground helper does no work until it is in reach.
//
// Reported: "A unit that is ordered to help a factory shouldn't start helping
// until it gets in range." A Construction Kbot (ARMCK) far from a Kbot Lab
// (ARMLAB) building a Peewee is told to help the output (HelpBuild, World::assist)
// or to guard the lab (Follow_Ground 0x407f90, which puts a HelpBuild on its
// output ahead of the guard). Ground HelpBuild (TotalA.exe 0x405cf0) works only
// once its approach -- the ring 0x44d3b0 from half the site's diagonal out to
// builddistance more (stage 0, 0x403fd9) -- has arrived: until the helper's
// footprint enters that ring it asks the books for nothing (econBuildFraction
// stamps books.noCloakUntil on every step it asks for), has no work stance
// (StartBuilding, 0x4040d4) and emits no nano; then it calls StartBuilding
// before its first step, and it does work, standing at the ring's edge.
#include "sim/matchsetup.h"
#include "sim/retailconstruction.h"
#include "hpi/hpi.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
namespace {
using namespace ta::sim;
enum class Mode {Assist,Guard};
bool reachCase(const TypeRegistry& registry,Mode mode,bool legion,float distance) {
    const UnitType* lab=registry.find("armlab");const UnitType* pw=registry.find("armpw");
    const UnitType* ck=registry.find("armck");
    if(!lab || !pw || !ck) {std::printf("FAIL: no armlab/armpw/armck\n");return false;}
    UnitType output=*pw;output.buildTime=std::max(output.buildTime,4000.0f);
    World world;
    if(legion)world.setPathfindingMode(PathfindingMode::Legion);
    world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const float cx=112*16,cz=112*16;
    const int factory=world.spawn(lab,cx,cz,float(int16_t(lab->orientation))*6.28318530718f/65536.f,0);
    world.train(factory,&output,1);
    int site=0;
    for(int t=0;t<600 && !site;++t) {world.tick(1.f/30);site=world.unit(factory)->productionSiteId;}
    if(!site) {std::printf("FAIL: no output site\n");return false;}
    // Behind the lab (away from its door), `distance` px from its centre.
    const float sx=world.unit(site)->x.toFloat(),sz=world.unit(site)->z.toFloat();
    float dx=cx-sx,dz=cz-sz;const float len=std::max(1.f,std::sqrt(dx*dx+dz*dz));dx/=len;dz/=len;
    const int helper=world.spawn(ck,cx+dx*distance,cz+dz*distance,0.f,0);
    if(mode==Mode::Guard)world.guard(helper,factory,false);
    else world.assist(helper,site,false);
    // HelpBuild's ring (TotalA.exe 0x403fd9): builddistance + half the site's diagonal.
    const RetailRingGoal ring=retailHelpBuildRing(world.unit(site)->x,world.unit(site)->z,int16_t(output.footX),
        int16_t(output.footZ),ck->footX,ck->footZ,uint16_t(ck->buildDist));
    int firstWork=-1,entered=-1,early=0,workTicks=0;float firstGap=0;
    for(int t=0;t<3000;++t) {
        world.tick(1.f/30);
        const Unit* b=world.unit(helper);const Unit* s=world.unit(site);
        if(!s || !s->alive() || !s->underConstruction) break;
        if(entered<0 && ring.accepts(footprintOrigin(b->x,ck->footX),footprintOrigin(b->z,ck->footZ)))entered=t;
        // econBuildFraction stamps the worker's books on every build step it asks for.
        const bool worked=b->books.noCloakUntil==world.tickCount()-1+150 || b->books.noCloakUntil==world.tickCount()+150;
        if(entered<0) {
            // On its way: no build step, no stance (StartBuilding), no nano emission.
            if(worked || b->workScriptWorking || b->constructionEmissions[0])++early;
            continue;
        }
        if(!worked)continue;
        ++workTicks;
        if(firstWork<0) {
            firstWork=t;
            firstGap=std::sqrt(std::pow(b->x.toFloat()-s->x.toFloat(),2.f)+std::pow(b->z.toFloat()-s->z.toFloat(),2.f));
            if(!b->workScriptWorking)++early;   // StartBuilding precedes the first work (0x4040d4)
        }
    }
    // It stops at the ring's edge: it does not walk on until it touches the
    // site (the Kingdoms HelpBuild this engine had: 43 px here, the ring 62).
    const bool stoppedAtRing=firstGap>=float(ring.outerTolerance-16);
    const bool pass=firstWork>=0 && early==0 && firstWork>=entered && stoppedAtRing;
    std::printf("%s: mode=%s %s distance=%.0f entered=%d firstWork=%d gap=%.0f ring=%d..%d workTicks=%d early=%d hash=%016llx\n",
        pass?"PASS":"FAIL",mode==Mode::Guard?"guard":"assist",legion?"Legion":"Retail",distance,entered,firstWork,firstGap,
        ring.innerRadius,ring.outerTolerance,workTicks,early,(unsigned long long)world.stateHash());
    return pass;
}
// MobileBuild (0x403a20): a ground builder places a Solar Collector, calls
// StartBuilding (0x403de1) and works only once its script's INBUILDSTANCE is
// up (0x403df5), and the site gets finished -- a script whose StartBuilding
// waits for its legs to settle (ARMCOM) must hear its StopMoving while it is
// held at the job, and the site must not decay meanwhile.
bool mobileCase(const TypeRegistry& registry,const char* builderId,const char* siteId) {
    const UnitType* bt=registry.find(builderId);const UnitType* st=registry.find(siteId);
    if(!bt || !st) {std::printf("FAIL: no %s/%s\n",builderId,siteId);return false;}
    World world;
    world.setTerrain(std::vector<uint8_t>(256*256,100),256,256,20);
    world.setMapPlacementFeatures(std::vector<uint16_t>(256*256,0xffff),{});
    world.buildNavClasses(registry);world.setPathService(true);
    const int b=world.spawn(bt,1500,1500,0.f,0);
    world.queueBuild(b,st,1700,1500,false);
    int firstWork=-1,early=0,done=-1;
    for(int t=0;t<6000 && done<0;++t) {
        world.tick(1.f/30);
        const Unit* u=world.unit(b);
        const bool worked=u->books.noCloakUntil==world.tickCount()-1+150 || u->books.noCloakUntil==world.tickCount()+150;
        if(worked && firstWork<0) {firstWork=t;if(!u->workScriptWorking)++early;}
        for(const auto& v:world.units())
            if(v.type==st && v.alive() && !v.underConstruction) done=t;
    }
    const bool pass=done>=0 && firstWork>=0 && !early && !world.unit(b)->workScriptWorking;
    std::printf("%s: mobile builder=%s site=%s firstWork=%d done=%d early=%d\n",pass?"PASS":"FAIL",builderId,siteId,firstWork,done,early);
    return pass;
}
}
int main(int argc,char** argv) {
    if(argc<2)return 2;
    auto vfs=ta::hpi::mountRetailRoot(argv[1]);
    TypeRegistry registry;setupRegistry(registry,vfs);
    int failures=0,cases=0;
    for(bool legion:{false,true})for(Mode mode:{Mode::Assist,Mode::Guard})for(float d:{200.f,900.f}) {
        ++cases;if(!reachCase(registry,mode,legion,d))++failures;
    }
    for(const auto& [builder,site]:{std::pair{"armcom","armsolar"},{"armck","armsolar"},{"armcv","armsolar"},
                                     {"corcom","corsolar"},{"corck","corsolar"}}) {
        ++cases;if(!mobileCase(registry,builder,site))++failures;
    }
    std::printf("assist_reach: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
