#include "client/ordertrail.h"
#include "sim/retailreclaimarea.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <unordered_set>

using ta::sim::Fixed;
static void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static Fixed f(int v) {return Fixed::fromInt(v);}
static RenderOrder area(int x0,int z0,int x1,int z1) {
    RenderOrder o;o.goal=true;o.x=f(x0);o.z=f(z0);o.buildX=f(x1);o.buildZ=f(z1);
    o.reclaimArea=true;o.issuedTick=123;
    return o;
}
static ta::OrderTrailTarget tree(int id,int cx,int cz,int fx=1,int fz=1) {
    return {id,f(cx*16+fx*8),f(cz*16+fz*8),cx,cz,fx,fz,true};
}
static std::vector<int> reclaims(const std::vector<RenderOrder>& out) {
    std::vector<int> ids;for(const auto& o:out)if(o.reclaimFeat)ids.push_back(o.reclaimFeat);return ids;
}
int main()try {
    static_assert(sizeof(RenderOrder)<=64);
    ta::sim::UnitType builder;builder.footX=2;builder.footZ=2;
    std::vector<RenderOrder> out;
    auto sweep=area(32,32,300,300);
    RenderOrder active;active.goal=true;active.x=f(72);active.z=f(72);active.reclaimFeat=1;active.issuedTick=99;
    RenderOrder tail;tail.goal=true;tail.x=f(500);tail.z=f(700);tail.patrol=true;tail.issuedTick=150;
    std::vector<ta::OrderTrailTarget> targets{tree(1,4,4),tree(2,10,4),tree(-7,14,4),tree(8,50,50)};
    ta::expandOrderTrail(std::vector<RenderOrder>{active,sweep,tail},builder,0,f(16),f(16),targets,out);
    check(reclaims(out)==std::vector<int>({1,2,-7}),"remaining clear includes corpses, excludes active duplicates and outside targets");
    check(out.back().patrol && out.back().x==tail.x && out.back().z==tail.z,"area chain joins queued patrol");
    check(out[1].issuedTick==sweep.issuedTick,"predicted steps retain the area's bead clock");
    targets.erase(targets.begin()+1);
    ta::expandOrderTrail(std::vector<RenderOrder>{sweep,tail},builder,0,f(72),f(72),targets,out);
    check(reclaims(out)==std::vector<int>({1,-7}),"removed targets disappear on refresh");
    ta::expandOrderTrail(std::vector<RenderOrder>{tail},builder,0,f(72),f(72),targets,out);
    check(out.size()==1 && out[0].patrol,"finished/cleared area leaves only the queued tail");
    auto edge=area(112,64,112,64);
    targets={tree(9,6,4,2,2)};
    ta::expandOrderTrail(std::vector<RenderOrder>{edge},builder,0,f(112),f(64),targets,out);
    check(reclaims(out)==std::vector<int>({9}),"footprint tail is included even with the centre outside the box");
    std::puts("PASS: area clear chain, active child, corpse, boundary, completion and queued tail");

    const auto sig=ta::orderTrailSignature(std::vector<RenderOrder>{sweep,tail});
    auto changed=sweep;changed.areaApproached=true;
    check(sig!=ta::orderTrailSignature(std::vector<RenderOrder>{changed,tail}),"cache invalidates on area progress");
    RenderOrder path;path.x=f(3);path.z=f(4);
    check(sig==ta::orderTrailSignature(std::vector<RenderOrder>{path,sweep,path,tail}),"route-only waypoints do not rebuild the preview");
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
        ta::expandOrderTrail(std::vector<RenderOrder>{sweep},builder,0,px,pz,targets,out);
        std::vector<int> expected;std::unordered_set<int> done;
        for(;;) {
            px=Fixed::raw(std::clamp(px.v,sweep.x.v,sweep.buildX.v));
            pz=Fixed::raw(std::clamp(pz.v,sweep.z.v,sweep.buildZ.v));
            const auto at=[&](int32_t x,int32_t z)->const ta::OrderTrailTarget* {
                const int cx=x>>20,cz=z>>20;
                for(const auto& t:targets)if(!done.contains(t.id) && cx>=t.cellX && cx<t.cellX+t.footX &&
                    cz>=t.cellZ && cz<t.cellZ+t.footZ)return &t;
                return nullptr;
            };
            ta::sim::RetailReclaimArea native{sweep.x.v,sweep.z.v,sweep.buildX.v,sweep.buildZ.v};
            const auto pick=ta::sim::retailReclaimAreaTarget(native,px.v,pz.v,[&](int32_t x,int32_t z){return at(x,z)!=nullptr;});
            if(!pick)break;
            const auto* t=at(pick->first,pick->second);expected.push_back(t->id);done.insert(t->id);px=t->x;pz=t->z;
        }
        check(reclaims(out)==expected,"indexed preview must match native greedy selection");
    }
    std::puts("PASS: indexed clear planning matches native sampled rectangles");

    targets.clear();for(int z=0;z<100;++z)for(int x=0;x<100;++x)targets.push_back(tree(z*100+x+1,x*3,z*3));
    sweep=area(0,0,4800,4800);
    const auto start=std::chrono::steady_clock::now();
    ta::expandOrderTrail(std::vector<RenderOrder>{sweep,tail},builder,0,f(16),f(16),targets,out);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto ids=reclaims(out);check(ids.size()==10000 && std::unordered_set<int>(ids.begin(),ids.end()).size()==10000 &&
        out.back().patrol,"large area retains every target once and the final queued command");
    std::printf("PASS: 10,000-target chain in %.2f ms, compact orders %zu bytes\n",ms,sizeof(RenderOrder));
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
