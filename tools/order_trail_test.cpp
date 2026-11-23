#include "client/ordertrail.h"
#include "sim/retailreclaimarea.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <unordered_set>

using tak::sim::Fixed;
static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static Fixed f(int v) {return Fixed::fromInt(v);}
static RenderOrder area(int x0,int z0,int x1,int z1,const tak::sim::UnitType* type=nullptr) {
    RenderOrder o;o.goal=true;o.x=f(x0);o.z=f(z0);o.buildX=f(x1);o.buildZ=f(z1);
    o.buildType=type;o.manaBuildArea=bool(type);o.reclaimArea=!type;o.issuedTick=123;
    return o;
}
static tak::OrderTrailTarget tree(int id,int cx,int cz,int fx=1,int fz=1) {
    return {id,f(cx*16+fx*8),f(cz*16+fz*8),cx,cz,fx,fz,true};
}
static std::vector<int> reclaims(const std::vector<RenderOrder>& out) {
    std::vector<int> ids;for(const auto& o:out)if(o.reclaimFeat)ids.push_back(o.reclaimFeat);return ids;
}
int main()try {
    static_assert(sizeof(RenderOrder)<=64);
    tak::sim::UnitType builder;builder.footX=2;builder.footZ=2;
    std::vector<RenderOrder> out;
    auto sweep=area(32,32,300,300);
    RenderOrder active;active.goal=true;active.x=f(72);active.z=f(72);active.reclaimFeat=1;active.issuedTick=99;
    RenderOrder tail;tail.goal=true;tail.x=f(500);tail.z=f(700);tail.patrol=true;tail.issuedTick=150;
    std::vector<tak::OrderTrailTarget> targets{tree(1,4,4),tree(2,10,4),tree(-7,14,4),tree(8,50,50)};
    tak::expandOrderTrail(std::vector<RenderOrder>{active,sweep,tail},builder,0,f(16),f(16),targets,{},{},out);
    check(reclaims(out)==std::vector<int>({1,2,-7}),"remaining clear includes corpses, excludes active duplicates and outside targets");
    check(out.back().patrol && out.back().x==tail.x && out.back().z==tail.z,"area chain joins queued patrol");
    check(out[1].issuedTick==sweep.issuedTick,"predicted steps retain the area's bead clock");
    targets.erase(targets.begin()+1);
    tak::expandOrderTrail(std::vector<RenderOrder>{sweep,tail},builder,0,f(72),f(72),targets,{},{},out);
    check(reclaims(out)==std::vector<int>({1,-7}),"removed targets disappear on refresh");
    tak::expandOrderTrail(std::vector<RenderOrder>{tail},builder,0,f(72),f(72),targets,{},{},out);
    check(out.size()==1 && out[0].patrol,"finished/cleared area leaves only the queued tail");
    auto edge=area(112,64,112,64);
    targets={tree(9,6,4,2,2)};
    tak::expandOrderTrail(std::vector<RenderOrder>{edge},builder,0,f(112),f(64),targets,{},{},out);
    check(reclaims(out)==std::vector<int>({9}),"footprint tail is included even with the centre outside the box");
    std::puts("PASS: area clear chain, active child, corpse, boundary, completion and queued tail");

    tak::sim::UnitType lode;lode.id="aralode";lode.side="ARAMON";lode.footX=2;lode.footZ=2;
    tak::sim::UnitType divine=lode;divine.id="aramana";
    auto build=area(0,0,512,512,&lode);
    std::vector<tak::OrderTrailSite> sites{{f(64),f(64)},{f(128),f(64)},
        {f(192),f(64),false},{f(256),f(64),true,&lode,0,false},{f(700),f(64)}};
    targets={tree(11,8,4),tree(12,7,4),tree(13,12,4)};
    const std::vector<uint32_t> firstTried{0},threeTried{0,1,2};
    tak::expandOrderTrail(std::vector<RenderOrder>{build,tail},builder,0,f(40),f(64),targets,sites,firstTried,out);
    check(reclaims(out)==std::vector<int>({12,11}),"mana area includes all footprint clearing and excludes hidden work");
    size_t builds=0;for(const auto& o:out)if(o.buildType) {
        ++builds;check(o.buildX==f(128) && o.trailPreview && o.clickX==o.buildX,"preview marker sits at the pending build site");
    }
    check(builds==1 && out.back().patrol,"mana progress excludes completed, unknown, occupied and out-of-box sites");
    build.buildType=&divine;
    tak::expandOrderTrail(std::vector<RenderOrder>{build},builder,0,f(40),f(64),{},sites,threeTried,out);
    check(out.size()==1 && out[0].buildType==&divine && out[0].buildX==f(256),"own ordinary lodestone can be upgraded");
    sites[3].player=1;
    tak::expandOrderTrail(std::vector<RenderOrder>{build},builder,0,f(40),f(64),{},sites,threeTried,out);
    check(out.empty(),"cannot predict an enemy lodestone upgrade");
    sites[3].player=0;sites[3].underConstruction=true;
    tak::expandOrderTrail(std::vector<RenderOrder>{build},builder,0,f(40),f(64),{},sites,threeTried,out);
    check(out.empty(),"unfinished lodestone cannot be upgraded");
    build.buildType=&lode;build.areaExploring=true;
    RenderOrder scout;scout.goal=true;scout.x=f(64);scout.z=f(64);
    builder.canFly=true;
    tak::expandOrderTrail(std::vector<RenderOrder>{scout,build},builder,0,f(40),f(64),{},sites,{},out);
    check(out.size()==3 && !out[0].buildType && out[1].buildX==f(64) && out[1].x==f(64),"scouting retains the following build once, with a flying approach");
    std::puts("PASS: area builds, clearing, fog, progress, upgrades and flying/scouting approaches");

    // The preview follows the sim's greedy nearest-first deposit order (index
    // order is deliberately scrambled), from wherever the previous order ends.
    {
        builder.canFly=false;
        const std::vector<tak::OrderTrailSite> spread{{f(1600),f(400)},{f(400),f(400)},{f(1000),f(400)},{f(400),f(1200)}};
        const auto order=[&](std::span<const RenderOrder> queue,int x,int z,std::span<const uint32_t> tried) {
            tak::expandOrderTrail(queue,builder,0,f(x),f(z),{},spread,tried,out);
            std::vector<int> spots;
            for(const auto& o:out)if(o.buildType)for(size_t i=0;i<spread.size();++i)
                if(std::abs((spread[i].x-o.buildX).toFloat())<=16 && std::abs((spread[i].z-o.buildZ).toFloat())<=16)spots.push_back(int(i));
            return spots;
        };
        const auto wide=area(0,0,2000,2000,&lode);
        check(order(std::vector<RenderOrder>{wide},256,400,{})==std::vector<int>({1,2,0,3}),"preview: nearest first from the west");
        check(order(std::vector<RenderOrder>{wide},1800,400,{})==std::vector<int>({0,2,1,3}),"preview: nearest first from the east");
        RenderOrder move;move.goal=true;move.x=f(400);move.z=f(1400);
        check(order(std::vector<RenderOrder>{move,wide},256,400,{})==std::vector<int>({3,1,2,0}),
              "preview: a Shift-queued area starts from the previous order's end");
        const std::vector<uint32_t> tried{1};
        check(order(std::vector<RenderOrder>{wide},400,400,tried)==std::vector<int>({2,0,3}),
              "preview: deposits the area already tried are not shown again");
        std::puts("PASS: area-build preview is nearest first from the previous stop");
    }

    const auto sig=tak::orderTrailSignature(std::vector<RenderOrder>{build,tail});
    auto changed=build;changed.areaNextSpot=5;
    check(sig!=tak::orderTrailSignature(std::vector<RenderOrder>{changed,tail}),"cache invalidates on area progress");
    RenderOrder path;path.x=f(3);path.z=f(4);
    check(sig==tak::orderTrailSignature(std::vector<RenderOrder>{path,build,path,tail}),"route-only waypoints do not rebuild the preview");
    changed=build;changed.areaExploring=false;
    check(sig!=tak::orderTrailSignature(std::vector<RenderOrder>{changed,tail}),"cache invalidates when scouting ends");
    std::puts("PASS: area cache signature follows work, not navigator churn");

    // Compare the spatial selector to the existing native grid selector across
    // fractional boxes, multi-cell footprints, changing builder positions/ties.
    std::mt19937 rng(28);
    for(int run=0;run<40;++run) {
        targets.clear();
        for(int i=0;i<24;++i)targets.push_back(tree(i+1,int(rng()%24),int(rng()%24),int(rng()%3+1),int(rng()%3+1)));
        // Disjoint footprints avoid ambiguous overlapping map anchors.
        for(size_t i=0;i<targets.size();++i)for(size_t j=0;j<i;++j) {
            const auto& a=targets[i];const auto& b=targets[j];
            if(a.cellX<b.cellX+b.footX && a.cellX+a.footX>b.cellX && a.cellZ<b.cellZ+b.footZ && a.cellZ+a.footZ>b.cellZ) {
                targets.erase(targets.begin()+i);--i;break;
            }
        }
        sweep=area(24,24,384,384);sweep.x=Fixed::raw(sweep.x.v+int32_t(rng()%65536));
        sweep.z=Fixed::raw(sweep.z.v+int32_t(rng()%65536));
        Fixed px=f(int(rng()%430)),pz=f(int(rng()%430));
        tak::expandOrderTrail(std::vector<RenderOrder>{sweep},builder,0,px,pz,targets,{},{},out);
        std::vector<int> expected;std::unordered_set<int> done;
        for(;;) {
            px=Fixed::raw(std::clamp(px.v,sweep.x.v,sweep.buildX.v));
            pz=Fixed::raw(std::clamp(pz.v,sweep.z.v,sweep.buildZ.v));
            const auto at=[&](int32_t x,int32_t z)->const tak::OrderTrailTarget* {
                const int cx=x>>20,cz=z>>20;
                for(const auto& t:targets)if(!done.contains(t.id) && cx>=t.cellX && cx<t.cellX+t.footX &&
                    cz>=t.cellZ && cz<t.cellZ+t.footZ)return &t;
                return nullptr;
            };
            tak::sim::RetailReclaimArea native{sweep.x.v,sweep.z.v,sweep.buildX.v,sweep.buildZ.v};
            const auto pick=tak::sim::retailReclaimAreaTarget(native,px.v,pz.v,[&](int32_t x,int32_t z){return at(x,z)!=nullptr;});
            if(!pick)break;
            const auto* t=at(pick->first,pick->second);expected.push_back(t->id);done.insert(t->id);px=t->x;pz=t->z;
        }
        check(reclaims(out)==expected,"indexed preview must match native greedy selection");
    }
    std::puts("PASS: indexed clear planning matches native sampled rectangles");

    targets.clear();for(int z=0;z<100;++z)for(int x=0;x<100;++x)targets.push_back(tree(z*100+x+1,x*3,z*3));
    sweep=area(0,0,4800,4800);
    const auto start=std::chrono::steady_clock::now();
    tak::expandOrderTrail(std::vector<RenderOrder>{sweep,tail},builder,0,f(16),f(16),targets,{},{},out);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto ids=reclaims(out);check(ids.size()==10000 && std::unordered_set<int>(ids.begin(),ids.end()).size()==10000 &&
        out.back().patrol,"large area retains every target once and the final queued command");
    std::printf("PASS: 10,000-target chain in %.2f ms, compact orders %zu bytes\n",ms,sizeof(RenderOrder));
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
