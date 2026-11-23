// Current-view count deltas (src/sim/retailviewcounts.h): a counted footprint
// that moves changes only the cells whose admission differs, and the counts
// must end exactly as after retail's full removal and re-add
// (retailUpdateSight). One ctest per part:
//   viewcount_delta_test probe   the audit's operation count: flat ground,
//                                sight 320, eye at 100; a one-cell move and a
//                                +6 eye change
//   viewcount_delta_test moves   footprint level against the rebuild: varied
//                                terrain, sight shapes, moves, eyes, map edges
//   viewcount_delta_test world   World level: the delta path against the
//                                rebuild reference, serial and workers, through
//                                moves, flyers, gifts, construction, status
//                                locks, deaths, revealer expiry, sight changes
#include "sim/sim.h"
#include "sim/retailviewcounts.h"
#include "sim/matchsetup.h"
#include "net/protocol.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace tak;
using namespace tak::sim;

namespace {
int failures=0;
void check(bool ok,const std::string& label) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",label.c_str());failures+=!ok;
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        s+=0x9e3779b97f4a7c15ull;
        uint64_t z=s;z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;
        return z^(z>>31);
    }
    int below(int n) {return n>0 ? int(next()%uint64_t(n)) : 0;}
    int range(int lo,int hi) {return lo+below(hi-lo+1);}
};

// One count plane updated both ways, with the visits each way made.
struct Pair {
    int width,height;
    const std::vector<RetailExplorationHeight>& heights;
    std::vector<uint16_t> rebuild,delta;
    std::vector<uint32_t> stamp;
    uint32_t step=0;
    size_t rebuildVisits=0,deltaVisits=0;
    bool once=true;
    Pair(int w,int h,const std::vector<RetailExplorationHeight>& t)
        : width(w),height(h),heights(t),rebuild(size_t(w)*h),delta(size_t(w)*h),stamp(size_t(w)*h) {}
    void update(RetailSightFootprint& a,RetailSightFootprint& b,Fixed x,Fixed y,Fixed z,uint8_t sea) {
        ++step;
        auto heightAt=[&](int cx,int cz){return heights[size_t(cz)*width+cx];};
        retailUpdateSight(a,x,y,z,sea,false,width,height,3,heightAt,
            [&](int cx,int cz,int d,uint16_t mask) {
                ++rebuildVisits;once=once && mask==0;
                auto& c=rebuild[size_t(cz)*width+cx];c=uint16_t(c+d);
            });
        retailUpdateSightCounts(b,x,y,z,sea,width,height,3,heightAt,
            [&](int cx,int cz,int d,uint16_t mask) {
                ++deltaVisits;
                // Each cell at most once per update, and only with +-1.
                auto& s=stamp[size_t(cz)*width+cx];
                once=once && s!=step && (d==1 || d==-1) && mask==0;
                s=step;
                auto& c=delta[size_t(cz)*width+cx];c=uint16_t(c+d);
            });
    }
};

bool sameSight(const RetailSightFootprint& a,const RetailSightFootprint& b) {
    return a.x==b.x && a.z==b.z && a.eyeHeight==b.eyeHeight && a.distance==b.distance &&
           a.sightHeight==b.sightHeight && a.active==b.active;
}

size_t changedCells(const std::vector<uint16_t>& before,const std::vector<uint16_t>& after) {
    size_t n=0;
    for (size_t i=0;i<before.size();++i) n+=before[i]!=after[i];
    return n;
}

// The audit probe (perf follow-up item G): flat ground, sight 320 (radius 20
// cells), eye at height 100 -- the whole disc is admitted (1257 cells).
void auditProbe() {
    constexpr int width=64,height=64;
    const std::vector<RetailExplorationHeight> flat(size_t(width)*height,RetailExplorationHeight{0,0});
    Pair p(width,height,flat);
    RetailSightFootprint a,b;
    a.distance=b.distance=320;a.sightHeight=b.sightHeight=24;
    const Fixed x=Fixed::fromInt(32*32+16),z=Fixed::fromInt(32*32+16);
    p.update(a,b,x,Fixed::fromInt(100),z,0);
    check(p.rebuildVisits==1257 && p.deltaVisits==1257 && p.rebuild==p.delta,
          "probe: a new footprint adds the 1257-cell disc either way");
    auto before=p.delta;
    p.rebuildVisits=p.deltaVisits=0;
    p.update(a,b,x+Fixed::fromInt(32),Fixed::fromInt(100),z,0);
    const size_t moved=changedCells(before,p.delta);
    std::printf("probe one-cell move: rebuild %zu counter updates, delta %zu, cells changed %zu\n",
                p.rebuildVisits,p.deltaVisits,moved);
    check(p.rebuildVisits==2514 && p.deltaVisits==82 && moved==82 && p.rebuild==p.delta && sameSight(a,b),
          "probe: a one-cell move is 2514 counter updates rebuilt, 82 as deltas, same counts");
    before=p.delta;
    p.rebuildVisits=p.deltaVisits=0;
    p.update(a,b,x+Fixed::fromInt(32),Fixed::fromInt(106),z,0);
    const size_t lifted=changedCells(before,p.delta);
    std::printf("probe eye 100->106: rebuild %zu counter updates, delta %zu, cells changed %zu\n",
                p.rebuildVisits,p.deltaVisits,lifted);
    check(p.rebuildVisits==2514 && p.deltaVisits==0 && lifted==0 && p.rebuild==p.delta &&
          sameSight(a,b) && a.eyeHeight==130,
          "probe: an eye change of 6 is 2514 counter updates rebuilt, none as deltas");
    p.rebuildVisits=p.deltaVisits=0;
    p.update(a,b,x+Fixed::fromInt(32+15),Fixed::fromInt(110),z,0);   // cell 33 still, eye 134
    check(p.rebuildVisits==0 && p.deltaVisits==0,"probe: the same cell within 5 of the eye is no update");
    check(p.once,"probe: each delta cell visited once, +-1, no exploration mask");
}

std::vector<RetailExplorationHeight> terrain(Rng& r,int kind,int width,int height,uint8_t sea) {
    std::vector<RetailExplorationHeight> out(size_t(width)*height);
    if (kind==4) {   // arbitrary pairs, either one the lower
        for (auto& h:out) h={uint8_t(r.below(256)),uint8_t(r.below(256))};
        return out;
    }
    if (kind==0) {
        const uint8_t level=uint8_t(r.below(256));
        for (auto& h:out) h={level,level};
        return out;
    }
    // A raw 16px heightmap through the projected plane, as setTerrain builds it.
    const int rw=width*2,rh=height*2;
    std::vector<uint8_t> raw(size_t(rw)*rh);
    const double fx=0.05+0.25*r.below(100)/100,fz=0.05+0.25*r.below(100)/100;
    const int base=r.range(20,140),amp=r.range(5,110),step=r.range(4,24);
    for (int z=0;z<rh;++z)
        for (int x=0;x<rw;++x) {
            int h;
            if (kind==1) h=r.below(256);
            else if (kind==2) h=base+int(amp*std::sin(x*fx)*std::cos(z*fz));
            else h=base+((x/step+z/step)%3)*amp/2;   // terraces and cliffs
            raw[size_t(z)*rw+x]=uint8_t(std::clamp(h,0,255));
        }
    return retailExplorationHeights(rw,rh,sea,[&](int x,int z){return raw[size_t(z)*rw+x];});
}

void moves() {
    Rng r{0x6a09e667f3bcc909ull};
    size_t rebuildVisits=0,deltaVisits=0,updates=0;
    bool same=true,once=true;
    static const int distances[]={0,1,15,16,17,31,32,33,47,48,64,100,160,180,256,320,400,512,640,900,-5,-16,-40};
    for (int map=0;map<60;++map) {
        const int width=r.range(6,72),height=r.range(6,72),kind=map%5;
        const uint8_t sea=uint8_t(r.below(3)==0 ? 0 : r.range(1,120));
        const auto heights=terrain(r,kind,width,height,sea);
        Pair p(width,height,heights);
        // Several footprints share the plane, as a player's units do.
        for (int f=0;f<6;++f) {
            RetailSightFootprint a,b;
            a.distance=b.distance=int16_t(r.below(3) ? distances[r.below(int(std::size(distances)))]
                                                     : r.range(0,800));
            a.sightHeight=b.sightHeight=uint8_t(r.below(4)==0 ? 0 : r.below(5)==0 ? 255 : r.range(1,120));
            int px=r.range(-64,width*32+64),pz=r.range(-64,height*32+64),py=r.range(0,300);
            for (int s=0;s<160;++s) {
                const int kindOfMove=r.below(20);
                if (kindOfMove<9) {           // walking: a cell or two, small eye drift
                    px+=r.range(-48,48);pz+=r.range(-48,48);py+=r.range(-4,4);
                } else if (kindOfMove<12) {   // climbing or a flyer changing height
                    py+=r.range(-40,40);
                } else if (kindOfMove<14) {   // a fast flyer or a jump
                    px+=r.range(-400,400);pz+=r.range(-400,400);py=r.range(-20,320);
                } else if (kindOfMove<15) {   // off the map and back
                    px=r.below(2) ? r.range(-200,-1) : width*32+r.range(0,200);
                } else if (kindOfMove<16) {   // anywhere
                    px=r.range(-64,width*32+64);pz=r.range(-64,height*32+64);
                } else {                      // inside the cell
                    px+=r.range(-8,8);pz+=r.range(-8,8);py+=r.range(-6,6);
                }
                py=std::clamp(py,-50,400);
                p.update(a,b,Fixed::fromInt(px),Fixed::fromInt(py),Fixed::fromInt(pz),sea);
                ++updates;
                same=same && p.rebuild==p.delta && sameSight(a,b);
            }
        }
        rebuildVisits+=p.rebuildVisits;deltaVisits+=p.deltaVisits;once=once && p.once;
    }
    std::printf("moves: %zu updates, rebuild %zu counter updates, delta %zu\n",updates,rebuildVisits,deltaVisits);
    check(same,"moves: counts and cached footprints equal the rebuild after every update");
    check(once,"moves: each delta cell visited at most once per update, with +-1");
    check(deltaVisits*3<rebuildVisits,"moves: deltas make well under a third of the rebuild's counter updates");
}

// The per-cell admission against its pieces: the rebuild's traversal is the
// reference, the row spans may not drop a cell it admits.
void spans() {
    Rng r{0xbb67ae8584caa73bull};
    bool ok=true;
    for (int i=0;i<4000 && ok;++i) {
        const int width=r.range(1,40),height=r.range(1,40);
        std::vector<RetailExplorationHeight> heights(size_t(width)*height);
        for (auto& h:heights) h={uint8_t(r.below(256)),uint8_t(r.below(256))};
        RetailSightFootprint s;
        s.x=int16_t(r.range(-3,width+2));s.z=int16_t(r.range(-3,height+2));
        s.distance=int16_t(r.range(-40,400));s.sightHeight=uint8_t(r.below(3) ? r.below(256) : 0);
        s.eyeHeight=r.range(-20,500);
        const int radius=retailSightRadius(s);
        std::vector<uint8_t> admitted(size_t(width)*height,0),spanned(size_t(width)*height,0);
        auto heightAt=[&](int x,int z){return heights[size_t(z)*width+x];};
        retailSightFootprint(s,true,false,width,height,0,heightAt,
            [&](int x,int z,int,uint16_t){admitted[size_t(z)*width+x]=1;});
        // Rows in order, and again in a scrambled order: the half-width is
        // corrected from whatever row came before.
        RetailSightRows rows(s,radius,width),scrambled(s,radius,width);
        for (int z=0;z<height;++z) {
            int lo,hi,lo2,hi2;
            rows.span(z,lo,hi);
            scrambled.span((z*7+3)%height,lo2,hi2);
            scrambled.span(z,lo2,hi2);
            ok=ok && lo==lo2 && hi==hi2;
            for (int x=lo;x<=hi;++x) spanned[size_t(z)*width+x]=1;
        }
        for (size_t c=0;c<admitted.size();++c) ok=ok && (!admitted[c] || spanned[c]);
    }
    check(ok,"spans: every cell the traversal admits lies in its row span");
    bool roots=true;
    for (int v=0;v<5000000 && roots;v+=v<100000 ? 1 : 997) {
        const int s=retailIsqrt(v);
        roots=s*s<=v && (s+1)*(s+1)>v;
    }
    check(roots,"spans: retailIsqrt is the exact floor square root");
}

// The per-height limit tables against retailSightAdmits, for every terrain
// height and every squared distance a cell of the row spans can have.
void limits() {
    bool ok=true;
    size_t tested=0;
    const int distances[]={0,15,16,17,31,32,48,100,180,320,401,800};
    const int sightHeights[]={0,1,24,60,255};
    const int eyes[]={-40,0,1,5,30,99,100,101,180,255,256,300,511};
    for (int distance:distances)
        for (int sightHeight:sightHeights)
            for (int eye:eyes) {
                RetailSightFootprint s;
                s.x=0;s.z=0;s.distance=int16_t(distance);s.sightHeight=uint8_t(sightHeight);s.eyeHeight=eye;
                const int radius=retailSightRadius(s),radiusSquared=radius*radius;
                const float slope=retailSightSlope(s);
                RetailSightLimits limit;
                retailSightLimits(s,radiusSquared,slope,limit);
                for (int m=0;m<256;++m) {
                    // Either corner the lower one.
                    const RetailExplorationHeight h=m%2 ? RetailExplorationHeight{uint8_t(m),255}
                                                        : RetailExplorationHeight{255,uint8_t(m)};
                    auto at=[&](int,int){return h;};
                    for (int dx=0;dx<=radius;++dx)
                        for (int dz=0;dz<=dx;++dz) {
                            const int d2=dx*dx+dz*dz;
                            // In the row span: inside the disc, or the box below radius 2.
                            if (radius>=2 && d2>radiusSquared) continue;
                            ok=ok && (d2<=limit[size_t(m)])==retailSightAdmits(s,dx,dz,radiusSquared,slope,at);
                            ++tested;
                        }
                }
            }
    std::printf("limits: %zu cell admissions compared\n",tested);
    check(ok,"limits: the per-height limits admit exactly what retailSightAdmits does");
}

// World level: four worlds -- delta and rebuild, serial and workers -- driven
// identically must keep the same counts and state hash every tick.
void world() {
    constexpr int kMap=128;
    std::vector<uint8_t> raw(size_t(kMap)*kMap);
    for (int z=0;z<kMap;++z)
        for (int x=0;x<kMap;++x) {
            int h=(x*7+z*3)%180;
            if (x>40 && x<70 && z>30 && z<60) h=210;         // a plateau
            if (x>90 && z>90) h=30+((x+z)/6%2)*90;            // cliffs
            raw[size_t(z)*kMap+x]=uint8_t(h);
        }
    auto makeType=[](const char* id,int sight,int sightHeight,bool fly,bool still) {
        UnitType t{};
        t.name=t.id=id;t.maxHp=100;t.footX=t.footZ=1;
        t.maxVel=still ? Fixed() : Fixed::fromInt(1);t.canMove=!still;
        t.canFly=fly;t.sight=float(sight);t.sightHeight=uint8_t(sightHeight);
        return t;
    };
    std::vector<UnitType> types;
    types.push_back(makeType("walker",160,24,false,false));
    types.push_back(makeType("scout",400,40,false,false));
    types.push_back(makeType("flyer",320,16,true,false));
    types.push_back(makeType("tower",256,60,false,true));
    types.push_back(makeType("blind",8,10,false,false));
    types.push_back(makeType("flat",200,0,false,false));
    TypeRegistry registry;
    struct Run {std::unique_ptr<World> w;const char* name;};
    std::vector<Run> runs;
    for (int i=0;i<4;++i) {
        auto w=std::make_unique<World>();
        w->setSerialThreads(i<2);
        w->setViewCountsByRebuildForTest(i%2==1);
        w->setPlayerCount(4);
        w->setTeam(0,0);w->setTeam(1,0);w->setTeam(2,2);w->setTeam(3,3);
        w->player(3).automaticGates=true;   // a computer player: its dead are removed at once
        w->setTerrain(raw,kMap,kMap,40);
        for (int u=0;u<4200;++u)
            w->spawn(&types[size_t(u%int(types.size()))],float((u*37)%(kMap*16)),float((u*53)%(kMap*16)),0.f,u%4);
        runs.push_back({std::move(w),i==0 ? "delta serial" : i==1 ? "rebuild serial" : i==2 ? "delta workers" : "rebuild workers"});
    }
    bool same=true;
    int firstDiff=-1;
    size_t kills=0,gifts=0;
    Rng r{0x3c6ef372fe94f82bull};
    // 100 steps: a human player's dead keep their sight for 60 ticks, so the
    // first revealers expire inside the run.
    for (int step=0;step<100 && same;++step) {
        const uint64_t seed=r.next();
        for (auto& run:runs) {
            World& w=*run.w;
            Rng m{seed};
            w.setDoubleSight(step>=30 && step<40);
            if (step==24) w.setTeam(2,3);
            for (size_t i=0;i<w.units().size();++i) {
                Unit& u=w.units()[i];
                if (!u.alive()) continue;
                const int what=m.below(40);
                if (what<14) {
                    u.x=u.x+Fixed::fromInt(m.range(-40,40));u.z=u.z+Fixed::fromInt(m.range(-40,40));
                } else if (what<17) {
                    u.groundY=Fixed::fromInt(m.range(0,255));u.flightY=Fixed::fromInt(m.range(0,400));
                } else if (what<18) {
                    u.x=Fixed::fromInt(m.range(-100,kMap*16+100));u.z=Fixed::fromInt(m.range(-100,kMap*16+100));
                } else if (what<19) {
                    u.underConstruction=!u.underConstruction;
                } else if (what<20) {
                    u.stonedFor=m.below(2) ? m.range(1,4) : 0;
                } else if (what<21) {
                    u.frozenFor=m.below(2) ? m.range(1,4) : 0;
                } else if (what==21 && step%3==0 && step<70) {
                    u.hp=Fixed();   // the death sweep takes it next tick
                    kills+=&run==&runs[0];
                } else if (what==22 && (u.player==0 || u.player==1)) {
                    const int was=u.player;
                    net::Command c;c.kind=net::Cmd::GiveUnit;c.unitId=u.id;c.player=u.player;c.targetId=1-u.player;
                    applyCommand(w,registry,c);
                    gifts+=&run==&runs[0] && u.player!=was;
                }
            }
            w.tick(1.f/30);
        }
        const auto& counts=runs[0].w->viewCounts();
        const uint64_t hash=runs[0].w->stateHash();
        for (size_t i=1;i<runs.size();++i)
            if (runs[i].w->viewCounts()!=counts || runs[i].w->stateHash()!=hash) {
                same=false;firstDiff=step;
                std::fprintf(stderr,"%s differs from %s at step %d\n",runs[i].name,runs[0].name,step);
            }
    }
    size_t seen=0;
    for (auto v:runs[0].w->viewCounts()) seen+=v!=0;
    std::printf("world: %zu kills, %zu gifts, %zu seen cells at the end\n",kills,gifts,seen);
    check(same,"world: delta and rebuild, serial and workers: same counts and hash every tick"+
                (firstDiff>=0 ? " (first difference at step "+std::to_string(firstDiff)+")" : std::string()));
    check(seen>0 && kills>0 && gifts>0,"world: the planes are not empty; deaths and gifts happened");
}
} // namespace

int main(int argc,char** argv) {
    const std::string part=argc>1 ? argv[1] : "all";
    if (part=="probe" || part=="all") auditProbe();
    if (part=="moves" || part=="all") {spans();limits();moves();}
    if (part=="world" || part=="all") world();
    if (part!="all" && part!="probe" && part!="moves" && part!="world") {
        std::fprintf(stderr,"usage: viewcount_delta_test [probe|moves|world]\n");return 2;
    }
    return failures ? 1 : 0;
}
