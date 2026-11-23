#include "tnt/mapgen_internal.h"
#include "hpi/hpi.h"
#include "sim/sim.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <map>
#include <stdexcept>
#include <thread>

namespace tak::mapgen {
using namespace detail;

namespace {
struct NoManaRuins : std::runtime_error {
    NoManaRuins():std::runtime_error("Random map mana site has no space for ruins"){}
};
}

const char* layoutName(uint8_t layout) {
    switch (layout) {
        case Lakes: return "Lakes";
        case Islands: return "Islands";
        case Maze: return "Taros Maze";
        case Ports: return "Veruna Ports";
        case Riverlands: return "Aramon Riverlands";
        case Jungle: return "Zhon Clearings";
        case Highlands: return "Creon Highlands";
        case LavaFlows: return "Volcano Lava Flows";
        case IndustrialZone: return "Volcano Industrial Zone";
        default: return "Mainland";
    }
}

uint8_t themedLayout(uint8_t world) {
    constexpr uint8_t layouts[]={Riverlands,Maze,Ports,Jungle,Highlands,LavaFlows};
    return layouts[world % kMapTypes];
}
const char* scatterName(uint8_t level) {
    static const char* names[kScatterLevels]={"None","Minimum","Medium","High","Max"};
    return names[std::min<int>(level,kScatterLevels-1)];
}
int scatterPitch(uint8_t level) {
    // Max: a home economy reserves an 87x87-cell square (7569 cells) around its
    // three deposits; six spots in that square is one per ~1262 cells, a 36-cell
    // pitch. Each lower level halves the density (pitch x sqrt 2).
    constexpr int pitch[kScatterLevels]={0,100,71,50,36};
    return pitch[std::min<int>(level,kScatterLevels-1)];
}

bool automaticWater(uint8_t layout) {
    return layout==Islands || layout==Ports || layout==Riverlands || layout==Maze || layout==Highlands || layout==LavaFlows ||
           layout==IndustrialZone;
}

namespace {
using Point = std::pair<int,int>;
struct FeatureSize { int x=1,z=1; bool blocking=false; };

std::map<std::string,FeatureSize> featureSizes(const hpi::Vfs& vfs,uint8_t world) {
    auto paths=vfs.list(std::string("features/")+worldKey(world));
    std::sort(paths.begin(),paths.end());
    std::map<std::string,FeatureSize> out;
    for (const auto& path:paths) {
        if (!hpi::MountSet::key(path).ends_with(".tdf")) continue;
        const auto data=vfs.read(path);
        const auto root=tdf::parseText(std::string(data.begin(),data.end()),path);
        for (const auto& [name,node]:root.children)
            out[name]={std::clamp(int(node.numberOr("footprintx",1)),1,64),
                       std::clamp(int(node.numberOr("footprintz",1)),1,64),
                       node.numberOr("blocking",0)!=0};
    }
    return out;
}

// This is map validation, not a new movement algorithm. Use the engine's retail
// terrain classifier and footprint clearance, then flood four-way legal steps.
// The cells a foot-wide body can stand on (NavGrid::fits for each), asked once per
// flood set instead of once per step. Single-threaded: fits() rebuilds a dirty
// clearance cache on first use.
std::vector<uint8_t> passable(const sim::NavGrid& nav,int foot) {
    const int w=nav.width(),h=nav.height();
    std::vector<uint8_t> ok(size_t(w)*h);
    for (int z=0;z<h;++z) for (int x=0;x<w;++x) ok[size_t(z)*w+x]=nav.fits(x,z,foot);
    return ok;
}

// Walking distance in cells from start over a passable mask (-1 = unreachable).
// Shortest distances do not depend on visiting order, so floods run in any order
// or thread give the same bytes on every peer.
std::vector<int> flood(const std::vector<uint8_t>& ok,int w,int h,Point start) {
    std::vector<int> d(size_t(w)*h,-1),q;
    const int s=start.second*w+start.first;
    if (start.first<0||start.second<0||start.first>=w||start.second>=h||!ok[size_t(s)]) return d;
    q.reserve(d.size());q.push_back(s);d[size_t(s)]=0;
    for (size_t head=0;head<q.size();++head) {
        const int c=q[head],z=c/w,x=c-z*w,next=d[size_t(c)]+1;
        auto step=[&](int n) { if (d[size_t(n)]<0&&ok[size_t(n)]) { d[size_t(n)]=next;q.push_back(n); } };
        if (z>0) step(c-w);
        if (x+1<w) step(c+1);
        if (z+1<h) step(c+w);
        if (x>0) step(c-1);
    }
    return d;
}

// Integer square root (floor), for the generator's integer-only geometry.
int64_t isqrt(int64_t v) {
    if(v<=0)return 0;
    int64_t r=int64_t(1)<<((std::bit_width(uint64_t(v))-1)/2+1);
    while(true){const int64_t n=(r+v/r)/2;if(n>=r)return r;r=n;}
}

std::vector<int> distances(const sim::NavGrid& nav,Point start,int foot) {
    return flood(passable(nav,foot),nav.width(),nav.height(),start);
}

// One flood per start, in parallel (a flood per thread, at most one thread per core).
std::vector<std::vector<int>> distancesFrom(const sim::NavGrid& nav,const std::vector<Point>& starts,int foot) {
    const auto ok=passable(nav,foot);
    const int w=nav.width(),h=nav.height();
    std::vector<std::vector<int>> out(starts.size());
    const size_t threads=std::clamp<size_t>(std::thread::hardware_concurrency(),1,starts.size());
    std::atomic<size_t> nextStart{0};
    auto work=[&] { for (size_t k;(k=nextStart++)<starts.size();) out[k]=flood(ok,w,h,starts[k]); };
    std::vector<std::thread> pool;
    for (size_t t=1;t<threads;++t) pool.emplace_back(work);
    work();
    for (auto& t:pool) t.join();
    return out;
}

// Each island has a generous cell allocation, including offshore launch space.
// Mainland starts follow an evenly sampled octagon, rather than rounding each
// player down to one of eight compass directions (which disadvantaged odd counts).
std::vector<Point> planStarts(const Params& p) {
    std::vector<Point> out;
    const int w=p.widthCells,h=p.heightCells;
    for (int k=0;k<p.players;++k) {
        int x,z;
        if (p.layout==Islands) {
            const int cols=p.players<=4?2:3,rows=(p.players+cols-1)/cols;
            x=(2*(k%cols)+1)*w/(2*cols);z=(2*(k/cols)+1)*h/(2*rows);
        } else {
            const int scaled=(k*8000/p.players+int(p.seed%8)*1000)%8000,edge=scaled/1000,t=scaled%1000;
            const auto a=kCompass[edge],b=kCompass[(edge+1)%8];
            x=w/2+(a.first*(1000-t)+b.first*t)*(w*35/100)/1000000;
            z=h/2+(a.second*(1000-t)+b.second*t)*(h*35/100)/1000000;
        }
        // Centre a base between section corners. A 96-cell square of level ground
        // fits within the four corner intervals reserved below.
        x=std::clamp((x/32)*32+16,48,w-48);
        z=std::clamp((z/32)*32+16,48,h-48);
        out.emplace_back(x,z);
    }
    return out;
}
}

namespace detail {
bool startsSpacedForAnySeed(const Params& p) {
    for (uint64_t rotation = 0; rotation < 8; ++rotation) {
        Params q = p;
        q.seed = rotation;
        const auto starts = planStarts(q);
        for (size_t i = 0; i < starts.size(); ++i)
            for (size_t j = 0; j < i; ++j) {
                const int dx = starts[i].first - starts[j].first, dz = starts[i].second - starts[j].second;
                if (dx * dx + dz * dz < kMinStartGap * kMinStartGap) return false;
            }
    }
    return true;
}
}

namespace {
Result generateOnce(const Params& p,const hpi::Vfs& vfs) {
    Result r;r.starts=planStarts(p);
    for(size_t i=0;i<r.starts.size();++i)for(size_t j=0;j<i;++j) {
        const int dx=r.starts[i].first-r.starts[j].first,dz=r.starts[i].second-r.starts[j].second;
        if(dx*dx+dz*dz<kMinStartGap*kMinStartGap)throw std::runtime_error("Random map starts are too close");
    }
    auto& m=r.map;m.stockTerrain=true;
    const int w=p.widthCells,h=p.heightCells,sw=w/32,sh=h/32;
    const auto levels=kLevels[p.mapType];const int land=levels.land;
    m.width=w;m.height=h;m.blocksX=w/2;m.blocksY=h/2;m.seaLevel=levels.sea;
    const size_t cells=size_t(w)*h,blocks=size_t(w/2)*(h/2);
    std::vector<uint8_t> wet(size_t(sw+1)*(sh+1)),reserved(cells),claim(cells),obstacles(cells);
    struct CellState {size_t index;uint16_t feature;uint8_t reserved,claim,obstacle;};
    std::vector<CellState> undo;bool transaction=false;
    const auto remember=[&](size_t i) {
        if(transaction)undo.push_back({i,m.features[i],reserved[i],claim[i],obstacles[i]});
    };
    const auto mark=[&](int x,int z,int radius,uint8_t value) {
        for (int nz=std::max(0,z-radius);nz<=std::min(h-1,z+radius);++nz)
            for (int nx=std::max(0,x-radius);nx<=std::min(w-1,x+radius);++nx) {
                const size_t i=size_t(nz)*w+nx;
                if((reserved[i]|value)!=reserved[i]){remember(i);reserved[i]|=value;}
            }
    };
    const auto reserveRoute=[&](Point a,Point b,int radius) {
        const int steps=std::max(std::abs(b.first-a.first),std::abs(b.second-a.second));
        for (int i=0;i<=steps;++i)
            mark(a.first+(b.first-a.first)*i/std::max(1,steps),
                 a.second+(b.second-a.second)*i/std::max(1,steps),radius,1);
    };
    for (auto s:r.starts) {
        mark(s.first,s.second,43,2); // home economy: terrain is flat; scenery allowed only outside base/routes
        mark(s.first,s.second,14,5); // bit 4: walkable lane home (see scattered mana)
        if (p.layout!=Islands && p.layout!=Maze && p.layout!=Ports && p.layout!=IndustrialZone) reserveRoute(s,{w/2,h/2},8);
    }
    if (p.layout==Ports) for (size_t i=0;i<r.starts.size();++i) {
        // Coastal settlements surround a shared basin. Keep the perimeter dry;
        // radial routes would split the basin into disconnected ponds.
        const auto a=r.starts[i];
        reserveRoute(a,{a.first< w/2 ? 48:w-48,a.second},8);
    }
    if (p.layout==Ports) {
        reserveRoute({48,48},{w-48,48},8);reserveRoute({w-48,48},{w-48,h-48},8);
        reserveRoute({w-48,h-48},{48,h-48},8);reserveRoute({48,h-48},{48,48},8);
    }
    const int islandCols=p.players<=4?2:3;
    const int islandRows=(p.players+islandCols-1)/islandCols;
    const int islandRadius=48+32*std::max(0,(std::min(w/islandCols,h/islandRows)-192)/128);
    // Volcano Lava Flows: a lava river runs from a fifth of the way out to the map's
    // edge between each pair of neighbouring homes (the starts go round in order),
    // meandering; the homes meet in the dry middle. Directions are scaled by 1000.
    std::vector<std::pair<int64_t,int64_t>> flows;
    if (p.layout==LavaFlows) for (size_t k=0;k<r.starts.size();++k) {
        const auto a=r.starts[k],b=r.starts[(k+1)%r.starts.size()];
        const int64_t ax=a.first-w/2,az=a.second-h/2,bx=b.first-w/2,bz=b.second-h/2;
        const int64_t la=std::max<int64_t>(1,isqrt(ax*ax+az*az)),lb=std::max<int64_t>(1,isqrt(bx*bx+bz*bz));
        int64_t dx=ax*1000/la+bx*1000/lb,dz=az*1000/la+bz*1000/lb;
        if (dx*dx+dz*dz<200*200) {dx=-az*1000/la;dz=ax*1000/la;}   // opposite homes: square to them
        const int64_t l=std::max<int64_t>(1,isqrt(dx*dx+dz*dz));
        flows.emplace_back(dx*1000/l,dz*1000/l);
    }
    const auto lavaFlow=[&](int cx,int cz) {
        const int64_t px=cx-w/2,pz=cz-h/2,inner=std::min(w,h)/5;
        for (size_t k=0;k<flows.size();++k) {
            const auto [dx,dz]=flows[k];
            const int64_t along=(px*dx+pz*dz)/1000,across=(px*dz-pz*dx)/1000;
            if (along<inner) continue;
            const int64_t bend=(int64_t(noise(p.seed^0x6c617661ULL,int(along),int(k)*977,128))-128)*72/128;
            const int64_t half=28+int64_t(noise(p.seed^0x77696465ULL,int(along),int(k)*977,80))*20/255;
            if (std::abs(across-bend)<=half) return true;
        }
        return false;
    };
    // Volcano Industrial Zone: lava everywhere but the homes and a few plateau blocks
    // (two sections square) out in it for expansions, every one at least three
    // corners from the rest so a lava section, room for a walkway, runs between.
    std::vector<uint8_t> izDry(size_t(sw+1)*(sh+1),0);
    std::vector<Point> izDeadEnds;   // sections whose walkway ends in a pad: mana spots
    std::vector<Point> izPads;       // cut-off lava pads (the LAVA PADS amount): flyers' mana
    if (p.layout==IndustrialZone) {
        uint64_t rng=p.seed^0x696e647573ULL;
        const auto homeGap=[&](int cx,int cz) {   // corners from the nearest home square
            int gap=1<<20;
            for (auto [sx,sz]:r.starts)
                gap=std::min(gap,std::max(std::abs(cx*32-sx)-48,std::abs(cz*32-sz)-48)/32);
            return gap;
        };
        const int want=std::max<int>(p.players,sw*sh/40);
        for (int placed=0,attempt=0;placed<want&&attempt<want*60&&sw>4&&sh>4;++attempt) {
            const int x0=int(splitmix(rng)%uint64_t(sw-1)),z0=int(splitmix(rng)%uint64_t(sh-1));
            bool good=true;
            for (int z=z0-3;good&&z<=z0+5;++z) for (int x=x0-3;x<=x0+5;++x) {
                const bool inside=x>=x0&&x<=x0+2&&z>=z0&&z<=z0+2;
                if (inside&&(x>sw||z>sh||homeGap(x,z)<3)) {good=false;break;}
                if (x>=0&&z>=0&&x<=sw&&z<=sh&&izDry[size_t(z)*(sw+1)+x]) {good=false;break;}
            }
            if (!good) continue;
            for (int z=z0;z<=z0+2;++z) for (int x=x0;x<=x0+2;++x) izDry[size_t(z)*(sw+1)+x]=1;
            ++placed;
        }
    }
    // Coherent low-frequency water; legacy lakes stay inland, new lakes may
    // reach the map edge. Zero means genuinely dry. Amount is an intensity, not a coverage %.
    for (int z=0;z<=sh;++z) for (int x=0;x<=sw;++x) {
        bool water=p.layout==Islands;
        if (!automaticWater(p.layout)&&p.waterDensity) {
            const int threshold=p.layout==Lakes ? 75+int(p.waterDensity)*90/255 : 25+int(p.waterDensity)*115/255;
            water=int(fractal(p.seed,x*32,z*32,std::max(w,h)))<threshold;
        }
        if (p.layout==Ports) water=x>=3 && x<=sw-3 && z>=3 && z<=sh-3;
        if (p.layout==Riverlands) {
            const int bend=int(noise(p.seed,x*32,0,w/3)%3)-1;
            water=std::abs(z-sh/2-bend)<=1 || std::abs(x-sw/2)<=1;
        }
        if (p.layout==LavaFlows) water=lavaFlow(x*32,z*32);
        if (p.layout==IndustrialZone) water=!izDry[size_t(z)*(sw+1)+x];
        for (auto [sx,sz]:r.starts) {
            const int dx=std::abs(x*32-sx),dz=std::abs(z*32-sz);
            const int radius=p.layout==Islands?islandRadius:48;
            // Round larger islands without cutting into their identical home areas.
            if(dx<=radius&&dz<=radius&&(radius<=48||dx+dz<=2*radius-32))water=false;
        }
        wet[size_t(z)*(sw+1)+x]=uint8_t(water);
    }
    // Reserve broad dry corridors in the corner mask before stamping art. Never
    // flatten or carve a route through already-painted coastline/cliff terrain.
    if (p.layout!=Islands) for (int z=0;z<=sh;++z) for (int x=0;x<=sw;++x) {
        for (int dz=-24;dz<=24;dz+=8) for (int dx=-24;dx<=24;dx+=8) {
            const int nx=x*32+dx,nz=z*32+dz;
            if (nx>=0&&nz>=0&&nx<w&&nz<h&&(reserved[size_t(nz)*w+nx]&1))
                wet[size_t(z)*(sw+1)+x]=0;
        }
    }
    const auto cellCase=[&](int x,int z) {
        const size_t i=size_t(z)*(sw+1)+x;
        return wet[i]|(wet[i+1]<<1)|(wet[i+sw+1]<<2)|(wet[i+sw+2]<<3);
    };
    // Monotone cleanup terminates: each change clears a previously wet corner.
    bool changed=true;
    while (changed) {
        changed=false;
        for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
            const int c=cellCase(x,z);
            if (c==6||c==9) {wet[size_t(z)*(sw+1)+x+(c==6)]=0;changed=true;}
        }
    }
    m.heights.resize(cells);m.features.assign(cells,0xffff);
    m.tileKeys.resize(blocks);m.tileCols.resize(blocks);m.tileRows.resize(blocks);
    const auto art=kWorldArt[p.mapType];
    for (int z=0;z<h;++z) for (int x=0;x<w;++x)
        m.heights[size_t(z)*w+x]=cellCase(x/32,z/32)==15?0:uint8_t(land);
    for (int z=0;z<h/2;++z) for (int x=0;x<w/2;++x) {
        const bool sea=cellCase(x/16,z/16)==15;const int period=sea?art.seaK:art.groundK;
        const size_t i=size_t(z)*(w/2)+x;
        m.tileKeys[i]=sea?art.sea:art.ground;m.tileCols[i]=uint8_t(x%period);m.tileRows[i]=uint8_t(z%period);
    }
    // Lava blocks everything that moves on the ground, as retail marks it (0xfffc on
    // every cell of lava_512); the shore sections carry their own marks.
    if (lavaWorld(p.mapType)) for (int z=0;z<h;++z) for (int x=0;x<w;++x)
        if (cellCase(x/32,z/32)==15) {const size_t i=size_t(z)*w+x;m.features[i]=0xfffc;obstacles[i]=claim[i]=1;}
    const auto sizes=featureSizes(vfs,p.mapType);
    const auto sizeOf=[&](const std::string& name)->const FeatureSize* {
        auto it=sizes.find(hpi::MountSet::key(name));return it==sizes.end()?nullptr:&it->second;
    };
    const auto intern=[&](const std::string& name) {
        auto it=std::find(m.featureNames.begin(),m.featureNames.end(),name);
        if (it!=m.featureNames.end()) return uint16_t(it-m.featureNames.begin());
        m.featureNames.push_back(name);return uint16_t(m.featureNames.size()-1);
    };
    const auto putFeature=[&](int x,int z,const std::string& name,bool sacred=false) {
        const auto* f=sizeOf(name);
        if (!f||x<0||z<0||x+f->x>w||z+f->z>h) return false;
        remember(size_t(z)*w+x);
        m.features[size_t(z)*w+x]=intern(name);
        for (int dz=0;dz<f->z;++dz) for (int dx=0;dx<f->x;++dx) {
            const size_t i=size_t(z+dz)*w+x+dx;remember(i);claim[i]=1;
            if (f->blocking&&!sacred) obstacles[i]=1;
        }
        return true;
    };
    // Sorted path lookup and explicit variant order keep output independent of
    // archive enumeration and standard-library hash-table iteration order.
    SectionCache sections(vfs);
    const auto load=[&](const std::string& path)->const tnt::Map* {return sections.load(path);};
    const auto stamp=[&](const tnt::Map& piece,int x0,int z0) {
        for (int z=0;z<piece.height;++z) for (int x=0;x<piece.width;++x) {
            const size_t src=size_t(z)*piece.width+x,dst=size_t(z0+z)*w+x0+x;
            m.heights[dst]=piece.heights[src];
            const auto f=piece.features[src];
            if(f==0xfffb||f==0xfffc) {
                m.features[dst]=f;
                if(f==0xfffc)obstacles[dst]=claim[dst]=1;
            }
            if (f<piece.featureNames.size()&&!reserved[dst]) putFeature(x0+x,z0+z,piece.featureNames[f]);
        }
        for (int z=0;z<piece.blocksY;++z) for (int x=0;x<piece.blocksX;++x) {
            const size_t src=size_t(z)*piece.blocksX+x,dst=size_t(z0/2+z)*(w/2)+x0/2+x;
            m.tileKeys[dst]=piece.tileKeys[src];m.tileCols[dst]=piece.tileCols[src];m.tileRows[dst]=piece.tileRows[src];
        }
    };
    // A world with several interchangeable ground sections mixes them over dry land.
    if (art.fillCount) for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
        if (cellCase(x,z)!=0) continue;
        const int pick=1+int(latticeVal(p.seed^0x66696c6cULL,x,z)%uint32_t(art.fillCount));
        if (const auto* fill=load(art.fills+std::to_string(pick)+".tnt"); fill && fill->width==32 && fill->height==32)
            stamp(*fill,x*32,z*32);
    }
    const auto& kit=kShoreKit[p.mapType];
    for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
        const int role=kCaseRole[cellCase(x,z)];if(role<0)continue;
        const tnt::Map* piece=nullptr;
        const int variant=int(latticeVal(p.seed,x,z)%3);
        for(int v=0;v<3&&!piece;++v)
            piece=load(std::string(kit.dir)+kit.name[role]+kit.var[(variant+v)%3]+kit.ext);
        if(!piece||piece->width!=32||piece->height!=32)
            throw std::runtime_error("Random map needs the retail coastline sections for this world");
        stamp(*piece,x*32,z*32);
    }
    if (p.layout==IndustrialZone) {
        // A walkway maze over the lava, from retail's road sections: a random spanning
        // tree over one connected stretch of lava, a few loops, its branches cut back to
        // the plateau ramps (some left as dead ends), and lava pads dotted elsewhere.
        // Each plateau is reached by a ramp (trans_*) in place of a straight rim section.
        uint64_t rng=p.seed^0x77616c6b77617973ULL;
        constexpr int E=1,N=2,S=4,W=8;
        const auto lava=[&](int x,int z){return x>=0&&z>=0&&x<sw&&z<sh&&cellCase(x,z)==15;};
        std::vector<int> pool(size_t(sw)*sh,-1);int pools=0;
        for (int at=0;at<sw*sh;++at) {
            if (!lava(at%sw,at/sw)||pool[size_t(at)]>=0) continue;
            std::vector<int> queue{at};pool[size_t(at)]=pools;
            for (size_t head=0;head<queue.size();++head) {
                const int x=queue[head]%sw,z=queue[head]/sw;
                for (auto [dx,dz]:std::array<Point,4>{{{1,0},{0,1},{-1,0},{0,-1}}})
                    if (lava(x+dx,z+dz)&&pool[size_t((z+dz)*sw+x+dx)]<0) {pool[size_t((z+dz)*sw+x+dx)]=pools;queue.push_back((z+dz)*sw+x+dx);}
            }
            ++pools;
        }
        // Plateaus: dry corners joined 8-ways; the homes are those holding a start.
        std::vector<int> mesa(wet.size(),-1);int mesas=0;
        for (int at=0;at<int(wet.size());++at) {
            if (wet[size_t(at)]||mesa[size_t(at)]>=0) continue;
            std::vector<int> queue{at};mesa[size_t(at)]=mesas;
            for (size_t head=0;head<queue.size();++head) {
                const int x=queue[head]%(sw+1),z=queue[head]/(sw+1);
                for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
                    const int nx=x+dx,nz=z+dz;if(nx<0||nz<0||nx>sw||nz>sh)continue;
                    const int n=nz*(sw+1)+nx;
                    if (!wet[size_t(n)]&&mesa[size_t(n)]<0) {mesa[size_t(n)]=mesas;queue.push_back(n);}
                }
            }
            ++mesas;
        }
        // Ramp candidates: a straight rim (lava on one side) whose lava neighbour is open.
        struct Ramp {int x,z,mesa,pool,toward;const char* name;};
        std::vector<Ramp> ramps;
        for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
            const int c=cellCase(x,z);int nx=x,nz=z,corner,toward;const char* name;
            if (c==3) {nz=z-1;corner=(z+1)*(sw+1)+x;toward=N;name="trans_n";}
            else if (c==12) {nz=z+1;corner=z*(sw+1)+x;toward=S;name="trans_s";}
            else if (c==5) {nx=x-1;corner=z*(sw+1)+x+1;toward=W;name="trans_w";}
            else if (c==10) {nx=x+1;corner=z*(sw+1)+x;toward=E;name="trans_e";}
            else continue;
            if (lava(nx,nz)) ramps.push_back({x,z,mesa[size_t(corner)],pool[size_t(nz*sw+nx)],toward,name});
        }
        std::vector<int> homeMesa;
        for (auto [sx,sz]:r.starts) homeMesa.push_back(mesa[size_t((sz/32)*(sw+1)+sx/32)]);
        // The stretch of lava every home can ramp onto.
        int main=-1;
        for (int candidate=0;candidate<pools&&main<0;++candidate) {
            bool all=true;
            for (int home:homeMesa) {
                bool reached=false;
                for (const auto& ramp:ramps) reached|=ramp.mesa==home&&ramp.pool==candidate;
                all&=reached;
            }
            if (all) main=candidate;
        }
        if (main<0) throw std::runtime_error("Random industrial zone has no walkway between the homes");
        // Two ramps per home, one per other plateau, spread round each.
        std::vector<Ramp> chosen;
        for (int id=0;id<mesas;++id) {
            std::vector<Ramp> mine;
            for (const auto& ramp:ramps) if (ramp.mesa==id&&ramp.pool==main) mine.push_back(ramp);
            const int want=std::find(homeMesa.begin(),homeMesa.end(),id)!=homeMesa.end()?2:1;
            for (int got=0;got<want&&!mine.empty();) {
                const size_t pick=size_t(splitmix(rng)%mine.size());
                const auto ramp=mine[pick];mine.erase(mine.begin()+std::ptrdiff_t(pick));
                bool spaced=true;
                for (const auto& other:chosen) if (std::max(std::abs(other.x-ramp.x),std::abs(other.z-ramp.z))<3) spaced=false;
                if (spaced||got==0) {chosen.push_back(ramp);++got;}
            }
        }
        // Spanning tree over the main stretch by a seeded depth-first walk, then loops.
        std::vector<uint8_t> link(size_t(sw)*sh,0),inTree(size_t(sw)*sh,0);
        const auto join=[&](int x,int z,int dx,int dz) {
            const int bit=dx>0?E:dx<0?W:dz>0?S:N,back=dx>0?W:dx<0?E:dz>0?N:S;
            link[size_t(z*sw+x)]|=uint8_t(bit);link[size_t((z+dz)*sw+x+dx)]|=uint8_t(back);
        };
        const auto rampEnd=[](const Ramp& ramp){
            return Point{ramp.x+(ramp.toward==E?1:ramp.toward==W?-1:0),ramp.z+(ramp.toward==S?1:ramp.toward==N?-1:0)};
        };
        {
            const auto first=rampEnd(chosen.front());
            std::vector<Point> stack{first};inTree[size_t(first.second*sw+first.first)]=1;
            while (!stack.empty()) {
                const auto [x,z]=stack.back();std::vector<Point> next;
                for (auto [dx,dz]:std::array<Point,4>{{{1,0},{0,1},{-1,0},{0,-1}}})
                    if (lava(x+dx,z+dz)&&pool[size_t((z+dz)*sw+x+dx)]==main&&!inTree[size_t((z+dz)*sw+x+dx)]) next.emplace_back(dx,dz);
                if (next.empty()) {stack.pop_back();continue;}
                const auto [dx,dz]=next[size_t(splitmix(rng)%next.size())];
                join(x,z,dx,dz);inTree[size_t((z+dz)*sw+x+dx)]=1;stack.push_back({x+dx,z+dz});
            }
        }
        for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
            if (!inTree[size_t(z*sw+x)]) continue;
            if (lava(x+1,z)&&inTree[size_t(z*sw+x+1)]&&!(link[size_t(z*sw+x)]&E)&&splitmix(rng)%100<12) join(x,z,1,0);
            if (lava(x,z+1)&&inTree[size_t((z+1)*sw+x)]&&!(link[size_t(z*sw+x)]&S)&&splitmix(rng)%100<12) join(x,z,0,1);
        }
        // Cut back every branch that leads nowhere, leaving one in five as a dead end.
        std::vector<uint8_t> keep(size_t(sw)*sh,0);
        for (const auto& ramp:chosen) {const auto [x,z]=rampEnd(ramp);keep[size_t(z*sw+x)]=1;}
        for (bool cut=true;cut;) {cut=false;
            for (int at=0;at<sw*sh;++at) {
                const uint8_t l=link[size_t(at)];
                if (keep[size_t(at)]||!(l==E||l==N||l==S||l==W)) continue;
                if (splitmix(rng)%100<20) {keep[size_t(at)]=1;continue;}
                const int x=at%sw,z=at/sw,dx=l==E?1:l==W?-1:0,dz=l==S?1:l==N?-1:0;
                link[size_t(at)]=0;link[size_t((z+dz)*sw+x+dx)]&=uint8_t(~(l==E?W:l==W?E:l==S?N:S));cut=true;
            }
        }
        for (const auto& ramp:chosen) {const auto [x,z]=rampEnd(ramp);
            link[size_t(z*sw+x)]|=uint8_t(ramp.toward==N?S:ramp.toward==S?N:ramp.toward==E?W:E);}
        const auto clearSection=[&](int x,int z) {
            for (int dz=0;dz<32;++dz) for (int dx=0;dx<32;++dx) {
                const size_t i=size_t(z*32+dz)*w+x*32+dx;m.features[i]=0xffff;obstacles[i]=claim[i]=0;
            }
        };
        // A walkway is a lane: scenery and ruins keep off it (reserved, as a route is).
        const auto place=[&](const std::string& path,int x,int z,bool lane) {
            const auto* piece=load(path);
            if (!piece||piece->width!=32||piece->height!=32) throw std::runtime_error("Industrial zone needs the retail volcano walkway sections");
            clearSection(x,z);stamp(*piece,x*32,z*32);
            if (lane) for (int dz=0;dz<32;++dz) for (int dx=0;dx<32;++dx) {
                const size_t i=size_t(z*32+dz)*w+x*32+dx;
                if (m.features[i]!=0xfffc&&m.heights[i]==land) reserved[i]|=1;
            }
        };
        for (int z=0;z<sh;++z) for (int x=0;x<sw;++x) {
            const uint8_t l=link[size_t(z*sw+x)];
            if (l) {
                std::string name;
                if (l==E||l==N||l==S||l==W) {name=std::string("end_")+(l==E?"e":l==N?"n":l==S?"s":"w");izDeadEnds.emplace_back(x,z);}
                else {
                    for (auto [bit,part]:std::array<std::pair<int,const char*>,4>{{{E,"e"},{N,"n"},{S,"s"},{W,"w"}}})
                        if (l&bit) name+=(name.empty()?"":"_")+std::string(part);
                    name+="01";
                }
                place(std::string(kIndustrialRoads)+name+".tnt",x,z,true);
            } else if (lava(x,z)&&splitmix(rng)%576<p.waterDensity) {   // the default 96: one section in six
                place(std::string(kIndustrialIsle)+std::to_string(1+int(splitmix(rng)%4))+".tnt",x,z,false);
                izPads.emplace_back(x,z);
            }
        }
        for (const auto& ramp:chosen) place(std::string(kIndustrialRoads)+ramp.name+".tnt",ramp.x,ramp.z,true);
        // ...and so does the plateau section a ramp climbs onto, so no ruin walls it in,
        // and a home keeps a lane from its start to each of its ramps.
        for (const auto& ramp:chosen) {
            const int x=ramp.x-(ramp.toward==E?1:ramp.toward==W?-1:0),z=ramp.z-(ramp.toward==S?1:ramp.toward==N?-1:0);
            for (size_t k=0;k<r.starts.size();++k)
                if (homeMesa[k]==ramp.mesa) reserveRoute(r.starts[k],{x*32+16,z*32+16},8);
            if (x<0||z<0||x>=sw||z>=sh) continue;
            for (int dz=0;dz<32;++dz) for (int dx=0;dx<32;++dx) {
                const size_t i=size_t(z*32+dz)*w+x*32+dx;
                if (m.heights[i]==land&&m.features[i]!=0xfffc) reserved[i]|=1;
            }
        }
    }
    // Themed uplands use complete authored cliff/hillside pieces, not painted
    // blockers or height-only walls. A low corner uses the shore kit's wet role.
    std::vector<uint8_t> high(wet.size(),0);
    if (p.layout==Maze || p.layout==Highlands) {
        high.assign(wet.size(),1);
        const auto carve=[&](int x,int z) {
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
                if(x+dx>=0&&z+dz>=0&&x+dx<=sw&&z+dz<=sh)
                    high[size_t(z+dz)*(sw+1)+x+dx]=0;
        };
        const auto corridor=[&](Point a,Point b) {
            reserveRoute({a.first*32,a.second*32},{b.first*32,a.second*32},8);
            reserveRoute({b.first*32,a.second*32},{b.first*32,b.second*32},8);
            while(a.first!=b.first) {carve(a.first,a.second);a.first+=a.first<b.first?1:-1;}
            while(a.second!=b.second) {carve(a.first,a.second);a.second+=a.second<b.second?1:-1;}
            carve(a.first,a.second);
        };
        if(p.layout==Maze) {
            // Seeded depth-first maze on a coarse lattice. Wide corridors admit
            // armies; home clearings connect to the nearest maze node.
            const int cols=(sw-6)/4+1,rows=(sh-6)/4+1;
            std::vector<uint8_t> seen(size_t(cols)*rows);
            std::vector<Point> stack{{0,0}};seen[0]=1;
            uint64_t rng=p.seed^0x4d415a45ULL;
            while(!stack.empty()) {
                auto [x,z]=stack.back(); std::vector<Point> next;
                for(auto [dx,dz]:std::array<Point,4>{{{1,0},{0,1},{-1,0},{0,-1}}})
                    if(x+dx>=0&&z+dz>=0&&x+dx<cols&&z+dz<rows&&!seen[size_t(z+dz)*cols+x+dx]) next.emplace_back(x+dx,z+dz);
                if(next.empty()) {stack.pop_back();continue;}
                auto n=next[splitmix(rng)%next.size()];seen[size_t(n.second)*cols+n.first]=1;
                corridor({3+4*x,3+4*z},{3+4*n.first,3+4*n.second});stack.push_back(n);
            }
            // Extend edge corridors to the boundary rather than enclosing the
            // entire battlefield in an unbroken cliff wall.
            {
                for(int x=0;x<cols;++x) {
                    corridor({3+4*x,3},{3+4*x,0});
                    corridor({3+4*x,3+4*(rows-1)},{3+4*x,sh});
                }
                for(int z=0;z<rows;++z) {
                    corridor({3,3+4*z},{0,3+4*z});
                    corridor({3+4*(cols-1),3+4*z},{sw,3+4*z});
                }
            }
            for(auto [x,z]:r.starts) corridor({x/32,z/32},
                {3+4*std::clamp((x/32-1)/4,0,cols-1),3+4*std::clamp((z/32-1)/4,0,rows-1)});
        } else {
            for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x)
                high[size_t(z)*(sw+1)+x]=p.reliefDensity && int(fractal(p.seed^0x48494748,x*32,z*32,std::max(w,h)))<80+int(p.reliefDensity)*100/255;
        }
        for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
            for(auto [sx,sz]:r.starts) if(std::abs(x*32-sx)<=48&&std::abs(z*32-sz)<=48) high[size_t(z)*(sw+1)+x]=0;
            if(p.layout==Highlands) for(int dz=-24;dz<=24;dz+=8)for(int dx=-24;dx<=24;dx+=8) {
                const int nx=x*32+dx,nz=z*32+dz;
                if(nx>=0&&nz>=0&&nx<w&&nz<h&&(reserved[size_t(nz)*w+nx]&1)) high[size_t(z)*(sw+1)+x]=0;
            }
        }
        const auto highCase=[&](int x,int z) {const size_t i=size_t(z)*(sw+1)+x;
            return high[i]|(high[i+1]<<1)|(high[i+sw+1]<<2)|(high[i+sw+2]<<3);};
        bool clean=false;while(!clean) {clean=true;
            for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {const int c=highCase(x,z);
                if(c==6||c==9) {high[size_t(z)*(sw+1)+x+(c==6)]=0;clean=false;}}
        }
        const bool maze=p.layout==Maze;
        const char* creonNames[]={"n","s","e","w","nw","ne","se","sw","n_w","n_e","s_w","s_e"};
        const auto* top=load(maze?"Sections/Taros/High Flats/high_ground.TNT":"sections/creon/flatties/highland512.tnt");
        if(!top||top->width!=32||top->height!=32) throw std::runtime_error("Themed map needs retail high-ground sections");
        for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {
            const int c=highCase(x,z);
            if(c==0) continue;
            const tnt::Map* piece=top;
            if(c!=15) {
                const int role=kCaseRole[15-c];
                const std::string path=maze?std::string("Sections/Taros/Low to High Cliffs/")+kShoreKit[Taros].name[role]+"01.TNT":
                    std::string("sections/creon/hillsides/")+creonNames[role]+"01.tnt";
                piece=load(path);
            }
            if(!piece||piece->width!=32||piece->height!=32) throw std::runtime_error("Themed map needs retail cliff/hillside sections");
            stamp(*piece,x*32,z*32);
        }
    }
    // ---- v10 retail kits: cliff-edged plateaus, mountain ranges and towns ----------
    // All three are assembled from whole retail sections by marching squares on the
    // section-corner grid, exactly like the coast. Each draws from its own random
    // stream and runs only when its amount is non-zero, so a recipe with all three
    // at zero is the map without kits. Plateau corners keep clear of homes, water and the
    // reserved army routes; the final walking-distance validation still decides.
    const auto& spec=kKitSpec[p.mapType];
    const int corners=(sw+1)*(sh+1);
    std::vector<uint8_t> kitCell(cells);        // 1: plateau/mountain section, 2: town
    std::vector<uint8_t> kitTop(cells);         // walkable plateau top (scenery, see below)
    const auto maskCase=[&](const std::vector<uint8_t>& mask,int x,int z) {const size_t i=size_t(z)*(sw+1)+x;
        return int(mask[i]|(mask[i+1]<<1)|(mask[i+sw+1]<<2)|(mask[i+sw+2]<<3));};
    const auto cleanPinches=[&](std::vector<uint8_t>& mask) {
        for(bool again=true;again;) {again=false;
            for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {const int c=maskCase(mask,x,z);
                if(c==6||c==9) {mask[size_t(z)*(sw+1)+x+(c==6)]=0;again=true;}}}
    };
    // Choose a variant whose west and north rims best meet what is already there,
    // then vary by position among equally good ones.
    const auto pick=[&](const std::vector<KitPiece>& pieces,int x0,int z0,uint64_t salt)->const KitPiece* {
        const KitPiece* best=nullptr;int bestScore=1<<30;
        for(size_t i=0;i<pieces.size();++i) {
            const auto& piece=*pieces[i].map;int seam=0;
            if(x0>0)for(int z=0;z<piece.height;++z)
                seam=std::max(seam,std::abs(int(piece.heights[size_t(z)*piece.width])-int(m.heights[size_t(z0+z)*w+x0-1])));
            if(z0>0)for(int x=0;x<piece.width;++x)
                seam=std::max(seam,std::abs(int(piece.heights[size_t(x)])-int(m.heights[size_t(z0-1)*w+x0+x])));
            const int score=(seam/8)*4096+int(latticeVal(p.seed^salt,x0*7+int(i),z0*13+int(i))*16+i)%4096;
            if(score<bestScore){bestScore=score;best=&pieces[i];}
        }
        return best;
    };
    const auto stampKit=[&](const tnt::Map& piece,int x0,int z0,uint8_t kind) {
        stamp(piece,x0,z0);
        for(int z=0;z<piece.height;++z)for(int x=0;x<piece.width;++x)kitCell[size_t(z0+z)*w+x0+x]=kind;
    };
    const auto complete=[](const CaseKit& kit,bool centre) {
        for(int c:{1,2,3,4,5,7,8,10,11,12,13,14})if(kit[size_t(c)].empty())return false;
        return !centre || !kit[15].empty();
    };
    const auto nearStart=[&](int x,int z,int d) {
        for(auto [sx,sz]:r.starts)if(std::abs(x*32-sx)<=d&&std::abs(z*32-sz)<=d)return true;
        return false;
    };
    // A corner may rise when no water corner touches it, it is clear of every home,
    // and no reserved route passes within 24 cells (sampled as Highlands does).
    std::vector<uint8_t> allowHigh(size_t(corners),0);
    for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
        bool ok=!nearStart(x,z,48);
        for(int dz=-1;ok&&dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
            const int nx=std::clamp(x+dx,0,sw),nz=std::clamp(z+dz,0,sh);
            if(wet[size_t(nz)*(sw+1)+nx]){ok=false;break;}
        }
        for(int dz=-24;ok&&dz<=24;dz+=8)for(int dx=-24;dx<=24;dx+=8) {
            const int nx=x*32+dx,nz=z*32+dz;
            if(nx>=0&&nz>=0&&nx<w&&nz<h&&(reserved[size_t(nz)*w+nx]&1)){ok=false;break;}
        }
        allowHigh[size_t(z)*(sw+1)+x]=uint8_t(ok);
    }
    const bool ownPlateaus=!automaticPlateaus(p.layout);
    CaseKit edges;
    if(ownPlateaus&&(p.plateauDensity||p.mountainDensity)&&kitAvailable(p.mapType,KitPlateaus)) {
        edges=edgeKit(sections,spec);
        CaseKit rims;   // every rim role needs a piece of the main family
        for(int c=0;c<16;++c)for(const auto& piece:edges[size_t(c)])if(!piece.alternate)rims[size_t(c)].push_back(piece);
        if(!complete(rims,false)||!load(spec.top))edges={};
    }
    const bool raise=!kitEmpty(edges);
    // Mountains: a named marching kit (Creon's on snow), or Zhon's standalone spires.
    CaseKit mountainKit=p.mountainDensity?namedKit(sections,spec.mountains,32,false):CaseKit{};
    if(spec.mountainFill&&mountainKit[15].empty())if(const auto* fill=load(spec.mountainFill))mountainKit[15].push_back({fill,false,false});
    if(!complete(mountainKit,true))mountainKit={};
    const auto peaks=p.mountainDensity?standaloneKit(sections,spec.peaks):std::vector<const tnt::Map*>{};
    CaseKit snowKit;
    if(!kitEmpty(mountainKit)&&spec.snowLine) {
        snowKit=namedKit(sections,spec.snowLine,16,false);
        const auto* fill=load(spec.snowFill);
        if(fill&&fill->width==16)snowKit[15].push_back({fill,false,false});
        if(!complete(snowKit,true))mountainKit={};
    }
    const bool rangeKit=!kitEmpty(mountainKit),spireKit=!peaks.empty();
    // A mountain core needs plateau all around it: one corner of plateau beyond the
    // range, two where a snow line must fit between the range and the plateau rim.
    const int moat=spec.snowLine?2:1;
    std::vector<uint8_t> mountain(size_t(corners),0),kitHigh(size_t(corners),0);
    uint64_t kitRng=p.seed^0x6b6974733130ULL;
    const int span=std::max(w,h);
    // Mountains are separate from the plain plateaus: a few seeds per map, each grown
    // by a short four-way walk into a single peak, a ridge or a clump, and raised on
    // a plateau of its own with a corner of open top around it (two on Creon, for the
    // snow line), so a range covers only part of its plateau and never merges into a
    // plain one. Maze and Highlands put them on high ground they already have.
    std::vector<uint8_t> mountainHigh(size_t(corners),0);
    if((rangeKit||spireKit)&&(raise||!ownPlateaus)&&p.mountainDensity) {
        const int margin=moat+1;
        const bool spires=spireKit&&!rangeKit;
        const int seeds=std::max(1,(int(p.mountainDensity)*sw*sh+255*32)/(255*64));   // at most one per 64 sections
        const int longest=spires?0:1+int(p.mountainDensity)/64;               // up to 1..4 extra cores
        // inner: the corners round a core that must rise. A full range also wants the
        // ring beyond them; a compact peak (the second pass, only when no range found
        // room) needs just the moat, its outer ring kept clear of every home.
        const auto fits=[&](int x,int z,bool compact) {
            const int inner=compact?moat:margin;
            if(x<inner||z<inner||x>sw-inner||z>sh-inner)return false;
            for(int dz=-margin-1;dz<=margin+1;++dz)for(int dx=-margin-1;dx<=margin+1;++dx) {
                const int nx=x+dx,nz=z+dz;if(nx<0||nz<0||nx>sw||nz>sh)continue;
                const size_t j=size_t(nz)*(sw+1)+nx;
                const int ring=std::max(std::abs(dx),std::abs(dz));
                const bool inside=ring<=inner;
                if(ownPlateaus) {
                    if(inside&&!allowHigh[j])return false;
                    if(compact&&ring==inner+1&&nearStart(nx,nz,48))return false;
                    if(kitHigh[j]||mountainHigh[j])return false;   // keep plain plateaus and other ranges apart
                } else {
                    if(inside&&!high[j])return false;
                    if(mountain[j])return false;
                }
            }
            return true;
        };
        static constexpr std::array<Point,4> steps={{{1,0},{0,1},{-1,0},{0,-1}}};
        int placed=0;
        for(int compact=0;compact<2&&!placed;++compact)
        for(int attempt=0;placed<seeds&&attempt<seeds*40&&sw>2*margin&&sh>2*margin;++attempt) {
            const int x0=margin+int(splitmix(kitRng)%uint64_t(sw-2*margin+1)),z0=margin+int(splitmix(kitRng)%uint64_t(sh-2*margin+1));
            if(!fits(x0,z0,compact))continue;
            std::vector<Point> path{{x0,z0}};
            const int length=longest&&!compact?int(splitmix(kitRng)%uint64_t(longest+1)):0;
            int dir=int(splitmix(kitRng)%4);
            for(int step=0;step<length;++step) {
                if(splitmix(kitRng)%3==0)dir=(dir+(splitmix(kitRng)%2?1:3))%4;   // ridges bend, clumps curl
                const Point next{path.back().first+steps[size_t(dir)].first,path.back().second+steps[size_t(dir)].second};
                if(std::find(path.begin(),path.end(),next)!=path.end()||!fits(next.first,next.second,false))break;
                path.push_back(next);
            }
            for(auto [x,z]:path) {
                mountain[size_t(z)*(sw+1)+x]=1;
                // The rings the range needs are whole; the open margin beyond them is
                // ragged, so the plateau's outline is irregular rather than a square.
                // A compact peak's margin rises only where plateau may.
                if(ownPlateaus)for(int dz=-margin;dz<=margin;++dz)for(int dx=-margin;dx<=margin;++dx) {
                    const int nx=x+dx,nz=z+dz;
                    if(compact&&(nx<0||nz<0||nx>sw||nz>sh))continue;
                    const size_t j=size_t(nz)*(sw+1)+nx;
                    if(std::max(std::abs(dx),std::abs(dz))<margin) mountainHigh[j]=1;
                    else if(splitmix(kitRng)%100<55&&(!compact||(allowHigh[j]&&!nearStart(nx,nz,48)))) mountainHigh[j]=1;
                }
            }
            ++placed;
        }
        // A diagonal pinch in the ragged margin is filled, never cleared: clearing
        // could cut into the rings the range needs. Filled corners lie inside the
        // margin squares already checked.
        for(bool again=true;again;) {again=false;
            for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {const int c=maskCase(mountainHigh,x,z);
                if(c==6||c==9) {mountainHigh[size_t(z)*(sw+1)+x+(c==9)]=1;again=true;}}}
        for(size_t i=0;i<mountainHigh.size();++i)if(mountainHigh[i])kitHigh[i]=1;
    }
    // Plain plateaus fill the eligible ground the mountains left, a corner clear of
    // every mountain plateau so the two never merge.
    const auto nearRange=[&](int x,int z) {
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
            const int nx=x+dx,nz=z+dz;
            if(nx>=0&&nz>=0&&nx<=sw&&nz<=sh&&mountainHigh[size_t(nz)*(sw+1)+nx])return true;
        }
        return false;
    };
    if(raise&&p.plateauDensity) {
        // Low-frequency blobs; the amount sets how much of the eligible ground rises.
        const int threshold=196-int(p.plateauDensity)*76/255;
        for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
            const size_t i=size_t(z)*(sw+1)+x;
            if(allowHigh[i]&&!nearRange(x,z)&&int(fractal(p.seed^0x706c617465617553ULL,x*32,z*32,span*3/2))>=threshold)kitHigh[i]=1;
        }
        // A lone corner would make a 2x2-section pimple; plateaus need some breadth.
        for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
            auto& corner=kitHigh[size_t(z)*(sw+1)+x];if(!corner)continue;
            int neighbours=0;
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
                if((dx||dz)&&x+dx>=0&&z+dz>=0&&x+dx<=sw&&z+dz<=sh)neighbours+=kitHigh[size_t(z+dz)*(sw+1)+x+dx];
            if(!neighbours)corner=0;
        }
        // Every start also gets a plain plateau of its own a few sections out, nearer
        // to it than to any other start, so high ground (and the mana it can carry)
        // is shared out fairly instead of being left to the noise. At least four
        // corners wide, so a hillside ramp has straight cliffs on either side.
        if(p.plateauDensity>=48) {
            uint64_t flankRng=p.seed^0x666c616e6b73ULL;
            for(size_t k=0;k<r.starts.size();++k)
                for(int attempt=0;attempt<120;++attempt) {
                    const auto [dx,dz]=kCompass[size_t(splitmix(flankRng)%8)];
                    const int reach=3+int(splitmix(flankRng)%10);   // corners from the start
                    const bool wide=splitmix(flankRng)%2;
                    int a=wide?4:3+int(splitmix(flankRng)%2),b=wide?3+int(splitmix(flankRng)%2):4;
                    if(attempt>=80)a=b=3;   // cramped: a smaller block (cliff-family ramps only)
                    const int cx=r.starts[k].first/32+dx*reach/1000,cz=r.starts[k].second/32+dz*reach/1000;
                    const int x0=cx-a/2,z0=cz-b/2;
                    if(x0<1||z0<1||x0+a>sw||z0+b>sh)continue;
                    bool good=true;
                    for(int z=z0;good&&z<z0+b;++z)for(int x=x0;x<x0+a;++x)
                        if(!allowHigh[size_t(z)*(sw+1)+x]||nearRange(x,z)){good=false;break;}
                    // Nearer to its own start than to any other, with room to spare.
                    const int mx=(x0*2+a-1)*16,mz=(z0*2+b-1)*16;
                    const auto d2=[&](Point s){return (s.first-mx)*(s.first-mx)+(s.second-mz)*(s.second-mz);};
                    for(size_t other=0;good&&other<r.starts.size();++other)
                        if(other!=k&&d2(r.starts[other])*10<d2(r.starts[k])*14)good=false;
                    if(!good)continue;
                    for(int z=z0;z<z0+b;++z)for(int x=x0;x<x0+a;++x)kitHigh[size_t(z)*(sw+1)+x]=1;
                    break;
                }
        }
    }
    if(raise) {
        cleanPinches(kitHigh);
        for(size_t i=0;i<kitHigh.size();++i)if(kitHigh[i])high[i]=1;
    }
    // Keep only cores still fully surrounded after cleanup; Zhon's spires are single
    // sections, so they become isolated cores, at most one per section.
    const auto& top=high;
    for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
        auto& core=mountain[size_t(z)*(sw+1)+x];if(!core)continue;
        for(int dz=-moat;core&&dz<=moat;++dz)for(int dx=-moat;dx<=moat;++dx) {
            const int nx=x+dx,nz=z+dz;
            if(nx<0||nz<0||nx>sw||nz>sh||!top[size_t(nz)*(sw+1)+nx]){core=0;break;}
        }
    }
    if(spireKit&&!rangeKit) {
        // One spire per core: thin the cores so no two share a section.
        for(int z=0;z<=sh;++z)for(int x=0;x<=sw;++x) {
            auto& core=mountain[size_t(z)*(sw+1)+x];if(!core)continue;
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
                if((dz||dx)&&(dz<0||(dz==0&&dx<0))&&x+dx>=0&&z+dz>=0&&x+dx<=sw&&mountain[size_t(z+dz)*(sw+1)+x+dx])core=0;
        }
    } else cleanPinches(mountain);
    // ---- plateau edges and ramps --------------------------------------------------
    // A ramp is the best piece the role offers: one of the cliff family the army class
    // climbs; else one of the cliff family builders and infantry climb (Aramon), which
    // keeps the rim seamless; only then a hillside builders climb (Taros, whose
    // hillsides are too steep for siege). Retail's hillsides interlock with each
    // other, not with cliffs: beside a cliff their slope ends in a hard edge.
    const auto rampTier=[](const KitPiece& piece) {
        return piece.alternate?(piece.climbable?1:0):piece.walkable?3:piece.climbable?2:0;
    };
    const auto bestTier=[&](int c){int best=0;for(const auto& piece:edges[size_t(c)])best=std::max(best,rampTier(piece));return best;};
    // A tier-1 (hillside) ramp is laid as the world's run of end pieces when it has one.
    const auto runOf=[&](int c) {
        std::vector<const KitPiece*> run;
        const int slot=c==3?0:c==5?1:c==10?2:3;
        const char* names=spec.rampRuns[size_t(slot)];
        if(!names)return run;
        std::string list(names);
        for(size_t at=0;at<=list.size();) {
            const size_t end=std::min(list.find(' ',at),list.size());
            const std::string want=list.substr(at,end-at);at=end+1;
            const KitPiece* found=nullptr;
            for(const auto& piece:edges[size_t(c)])if(piece.alternate&&piece.climbable&&piece.name==want)found=&piece;
            if(!found)return std::vector<const KitPiece*>{};   // incomplete run: none at all
            run.push_back(found);
        }
        return run;
    };
    std::vector<uint8_t> ramp(size_t(sw)*sh,0);
    std::vector<int> component(size_t(corners),-1);   // plateau each kit-high corner belongs to
    if(raise) {
        // Each plateau gets walkable ramps, retail hillside/ramp pieces that the
        // validation class can climb, spaced around its rim. Worlds whose edge kit
        // is all slope (Creon) are walkable everywhere anyway.
        int components=0;
        for(int start=0;start<corners;++start) {
            if(!kitHigh[size_t(start)]||component[size_t(start)]>=0)continue;
            std::vector<int> queue{start};component[size_t(start)]=components;
            for(size_t head=0;head<queue.size();++head) {
                const int x=queue[head]%(sw+1),z=queue[head]/(sw+1);
                for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
                    const int nx=x+dx,nz=z+dz;if(nx<0||nz<0||nx>sw||nz>sh)continue;
                    const int n=nz*(sw+1)+nx;
                    if(kitHigh[size_t(n)]&&component[size_t(n)]<0){component[size_t(n)]=components;queue.push_back(n);}
                }
            }
            std::vector<std::pair<uint64_t,int>> candidates;
            for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {
                const int c=maskCase(high,x,z);
                if(c!=3&&c!=5&&c!=10&&c!=12)continue;
                const int tier=bestTier(c);
                if(!tier||component[size_t((z+(c==3?0:1))*(sw+1)+x+(c==10?1:0))]!=components)continue;
                // A hillside ramp has no corner pieces of its own and shows a seam
                // against a cliff corner: it goes only between straight cliffs, as a
                // whole run when the world has one (x,z is the run's first section).
                const bool across=c==3||c==12;
                const int length=tier==1?std::max<int>(1,int(runOf(c).size())):1;
                bool room=true;
                for(int i=(tier==1?-1:0);room&&i<=(tier==1?length:length-1);++i) {
                    const int rx=x+(across?i:0),rz=z+(across?0:i);
                    if(rx<0||rz<0||rx>=sw||rz>=sh||maskCase(high,rx,rz)!=c){room=false;break;}
                    if(i<0||i>=length)continue;
                    // Every ramp section climbs onto open plateau, not a mountain.
                    const int tx=rx+(c==5?-1:c==10?1:0),tz=rz+(c==3?-1:c==12?1:0);
                    if(tx<0||tz<0||tx>=sw||tz>=sh||maskCase(high,tx,tz)!=15||maskCase(mountain,tx,tz))room=false;
                }
                if(!room)continue;
                // The first ramp faces the nearest start, so a plateau beside a home
                // is climbed from that home's side.
                uint64_t nearest=~0ull;
                for(auto [sx,sz]:r.starts) {
                    const int64_t ddx=x*32+16-sx,ddz=z*32+16-sz;
                    nearest=std::min<uint64_t>(nearest,uint64_t(ddx*ddx+ddz*ddz));
                }
                candidates.push_back({(nearest<<8)|(splitmix(kitRng)&255),z*sw+x});
            }
            bool ranged=false;for(int at:queue)ranged|=mountain[size_t(at)]!=0;
            r.mountainPlateaus+=ranged;
            std::sort(candidates.begin(),candidates.end());
            const int want=std::min(4,1+int(queue.size())/10);int placed=0;
            for(auto [order,at]:candidates) {
                if(placed==want)break;
                const int x=at%sw,z=at/sw,c=maskCase(high,x,z),tier=bestTier(c);
                const bool across=c==3||c==12;
                const int length=tier==1?std::max<int>(1,int(runOf(c).size())):1;
                bool spaced=true;
                for(int i=0;spaced&&i<length;++i)
                    for(int dz=-2;spaced&&dz<=2;++dz)for(int dx=-2;dx<=2;++dx) {
                        const int rx=x+(across?i:0)+dx,rz=z+(across?0:i)+dz;
                        if(rx>=0&&rz>=0&&rx<sw&&rz<sh&&ramp[size_t(rz)*sw+rx]){spaced=false;break;}
                    }
                if(!spaced)continue;
                // ramp holds 1 + the section's place in its run.
                for(int i=0;i<length;++i)ramp[size_t(z+(across?0:i))*sw+x+(across?i:0)]=uint8_t(1+i);
                ++placed;
            }
            ++components;
        }
        r.plateaus=components;
        for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {
            const int c=maskCase(high,x,z);
            if(!c||!maskCase(kitHigh,x,z))continue;
            if(c==15){stampKit(*load(spec.top),x*32,z*32,1);continue;}
            // Rims come from the main edge family (cliffs, preferring the ones an army
            // cannot climb); a ramp takes a piece of the role's best ramp tier.
            // Hillsides are never plain rim: they would meet the cliffs along mixed edges.
            const bool isRamp=ramp[size_t(z)*sw+x];
            std::vector<KitPiece> choice;
            const int tier=bestTier(c);
            const auto run=isRamp&&tier==1?runOf(c):std::vector<const KitPiece*>{};
            if(!run.empty())choice.push_back(*run[size_t(std::min<int>(ramp[size_t(z)*sw+x],int(run.size()))-1)]);
            else for(const auto& piece:edges[size_t(c)])
                if(isRamp?rampTier(piece)==tier:(!piece.alternate&&!piece.walkable))choice.push_back(piece);
            if(choice.empty())for(const auto& piece:edges[size_t(c)])if(!piece.alternate)choice.push_back(piece);
            if(choice.empty())choice=edges[size_t(c)];
            stampKit(*pick(choice,x*32,z*32,0x72696d)->map,x*32,z*32,1);
        }
    }
    // ---- mountain ranges and spires ------------------------------------------------
    if(rangeKit||spireKit) {
        std::vector<uint8_t> range(size_t(sw)*sh,0);
        for(int z=0;z<sh;++z)for(int x=0;x<sw;++x)range[size_t(z)*sw+x]=maskCase(mountain,x,z)?1:0;
        if(spec.snowLine&&rangeKit) {
            // Creon's ranges stand on snow: snow covers each range section and half a
            // section around it, and the 256px snow-line kit blends it into the plateau.
            const int sw2=sw*2,sh2=sh*2;std::vector<uint8_t> snow(size_t(sw2+1)*(sh2+1),0),grown;
            for(int z=0;z<sh;++z)for(int x=0;x<sw;++x)if(range[size_t(z)*sw+x])
                for(int dz=0;dz<=2;++dz)for(int dx=0;dx<=2;++dx)snow[size_t(z*2+dz)*(sw2+1)+x*2+dx]=1;
            grown=snow;
            for(int z=0;z<=sh2;++z)for(int x=0;x<=sw2;++x)if(snow[size_t(z)*(sw2+1)+x])
                for(auto [dx,dz]:std::array<Point,4>{{{1,0},{-1,0},{0,1},{0,-1}}})   // rounded, not square
                    if(x+dx>=0&&z+dz>=0&&x+dx<=sw2&&z+dz<=sh2)grown[size_t(z+dz)*(sw2+1)+x+dx]=1;
            for(int z=0;z<sh2;++z)for(int x=0;x<sw2;++x) {
                if(range[size_t(z/2)*sw+x/2])continue;
                const size_t i=size_t(z)*(sw2+1)+x;
                const int c=grown[i]|(grown[i+1]<<1)|(grown[i+sw2+1]<<2)|(grown[i+sw2+2]<<3);
                if(c&&!snowKit[size_t(c)].empty())stamp(*pick(snowKit[size_t(c)],x*16,z*16,0x736e6f77)->map,x*16,z*16);
            }
        }
        // Ranges count as connected groups of range sections.
        std::vector<uint8_t> seen(range.size(),0);
        for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {
            if(!range[size_t(z)*sw+x])continue;
            const int c=maskCase(mountain,x,z);
            if(rangeKit) stampKit(*pick(mountainKit[size_t(c)],x*32,z*32,0x6d746e)->map,x*32,z*32,1);
            else if(c==1||c==2||c==4||c==8) {
                // A spire core touches four sections; the spire stands in the one
                // to the core's south-east, which is plateau all around.
                if(c!=1)continue;
                stampKit(*peaks[size_t(splitmix(kitRng)%peaks.size())],x*32,z*32,1);
                ++r.mountains;continue;
            } else continue;
            if(seen[size_t(z)*sw+x])continue;
            ++r.mountains;std::vector<int> queue{z*sw+x};seen[size_t(z)*sw+x]=1;
            for(size_t head=0;head<queue.size();++head) {
                const int qx=queue[head]%sw,qz=queue[head]/sw;
                for(auto [dx,dz]:std::array<Point,4>{{{1,0},{-1,0},{0,1},{0,-1}}}) {
                    const int nx=qx+dx,nz=qz+dz;
                    if(nx<0||nz<0||nx>=sw||nz>=sh||!range[size_t(nz)*sw+nx]||seen[size_t(nz)*sw+nx])continue;
                    seen[size_t(nz)*sw+nx]=1;queue.push_back(nz*sw+nx);
                }
            }
        }
    }
    // Scenery may dress open plateau tops the kits built (Maze and Highlands keep
    // their own look); mana stays on the low ground, where every player can reach it.
    for(int z=0;z<sh;++z)for(int x=0;x<sw;++x) {
        if(maskCase(kitHigh,x,z)!=15||kitCell[size_t(z*32)*w+x*32]!=1)continue;
        bool open=true;   // not a range section, nor (on snow) beside one
        const int ring=spec.snowLine?1:0;
        for(int dz=-ring;open&&dz<=ring;++dz)for(int dx=-ring;dx<=ring;++dx)
            if(x+dx>=0&&z+dz>=0&&x+dx<sw&&z+dz<sh&&maskCase(mountain,x+dx,z+dz)){open=false;break;}
        if(open)for(int dz=0;dz<32;++dz)for(int dx=0;dx<32;++dx)kitTop[size_t(z*32+dz)*w+x*32+dx]=1;
    }
    // ---- towns ---------------------------------------------------------------------
    // Retail settlements: the walled-town kit around paved road, filled with the
    // world's cobbles and houses with their real TDF footprints. The painted walls carry no blockers, as in retail. Zhon
    // has no settlement sections, so its villages are tents, huts and a bonfire.
    if(p.townDensity&&kitAvailable(p.mapType,KitTowns)) {
        const CaseKit lowKit=namedKit(sections,spec.lowTowns,32,true);
        const CaseKit highKit=namedKit(sections,spec.highTowns,32,true);
        const tnt::Map* lowFill=spec.lowTownFill?load(spec.lowTownFill):nullptr;
        const tnt::Map* highFill=spec.highTownFill?load(spec.highTownFill):nullptr;
        const bool lowWalls=complete(lowKit,false)&&lowFill,highWalls=complete(highKit,false)&&highFill&&raise;
        const bool village=!lowWalls&&spec.buildings.n>0&&!spec.lowTowns;
        const int want=std::max(1,int(p.townDensity)*sw*sh/(255*40));
        std::vector<uint8_t> used(size_t(sw)*sh,0);
        for(int town=0;town<want&&(lowWalls||village||highWalls);++town) {
            for(int attempt=0;attempt<48;++attempt) {
                // Corner rectangle a x b; its sections form an (a+1) x (b+1) town.
                // Prefer 3x3 to 4x4-section towns; small ones only when nothing larger fits.
                const int smallest=attempt<32?2:1;
                const int a=smallest+int(splitmix(kitRng)%uint64_t(4-smallest)),b=smallest+int(splitmix(kitRng)%uint64_t(4-smallest));
                const int tx=1+int(splitmix(kitRng)%uint64_t(std::max(1,sw-a))),tz=1+int(splitmix(kitRng)%uint64_t(std::max(1,sh-b)));
                if(tx+a>sw||tz+b>sh)continue;
                const bool onHigh=highWalls&&(town%2==1||!lowWalls);
                bool good=true;
                for(int z=tz-1;good&&z<tz+b;++z)for(int x=tx-1;x<tx+a;++x) {
                    // Space the towns a section apart, on plain ground of one level.
                    for(int dz=-1;good&&dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
                        if(x+dx>=0&&z+dz>=0&&x+dx<sw&&z+dz<sh&&used[size_t(z+dz)*sw+x+dx]){good=false;break;}
                    if(!good)break;
                    if(cellCase(x,z)||maskCase(mountain,x,z)){good=false;break;}
                    if(onHigh?(maskCase(kitHigh,x,z)!=15||maskCase(high,x,z)!=15):maskCase(high,x,z)!=0){good=false;break;}
                    for(int dz=0;good&&dz<32;dz+=4)for(int dx=0;dx<32;dx+=4) {
                        const size_t i=size_t(z*32+dz)*w+x*32+dx;
                        if((reserved[i]&2)||claim[i]||(!onHigh&&kitCell[i])||m.heights[i]!=(onHigh?spec.high:land)){good=false;break;}
                    }
                }
                if(!good)continue;
                for(int z=tz-1;z<tz+b;++z)for(int x=tx-1;x<tx+a;++x)used[size_t(z)*sw+x]=1;
                const int x0=(tx-1)*32,z0=(tz-1)*32,x1=(tx+a)*32,z1=(tz+b)*32;
                if(!village) {
                    const CaseKit& kit=onHigh?highKit:lowKit;
                    for(int z=tz-1;z<tz+b;++z)for(int x=tx-1;x<tx+a;++x) {
                        // A section's corners inside the corner rectangle are paved.
                        const int c=(z>=tz&&x>=tx?1:0)|(z>=tz&&x+1<tx+a?2:0)|(z+1<tz+b&&x>=tx?4:0)|(z+1<tz+b&&x+1<tx+a?8:0);
                        if(c==0)continue;
                        if(c==15){stampKit(onHigh?*highFill:*lowFill,x*32,z*32,2);continue;}
                        // Retail gate pieces carry a road out to the section edge, where
                        // it would stop dead in the grass; plain walls are used instead.
                        std::vector<KitPiece> choice;
                        for(const auto& piece:kit[size_t(c)])if(!piece.gate)choice.push_back(piece);
                        if(choice.empty())continue;
                        stampKit(*pick(choice,x*32,z*32,0x746f776e)->map,x*32,z*32,2);
                    }
                }
                // Houses go on the paved inside, clear of the wall band and of every
                // reserved lane; a village rings its bonfire with tents and huts.
                const int area=(x1-x0)*(z1-z0),houses=std::max(2,area/(village?700:420));
                const int level=onHigh?spec.high:land;
                const auto paved=[&](int x,int z,int fx,int fz,int margin) {
                    for(int dz=-margin;dz<fz+margin;++dz)for(int dx=-margin;dx<fx+margin;++dx) {
                        const int nx=x+dx,nz=z+dz;
                        if(nx<x0||nz<z0||nx>=x1||nz>=z1)return false;
                        const size_t i=size_t(nz)*w+nx;
                        if(claim[i]||(reserved[i]&1)||m.heights[i]!=level||(!village&&m.features[i]!=0xfffb))return false;
                    }
                    return true;
                };
                const int cx=(x0+x1)/2,cz=(z0+z1)/2;
                if(village&&spec.buildings.n>0) {
                    const char* fire=spec.buildings.p[spec.buildings.n-1];
                    if(const auto* f=sizeOf(fire);f&&paved(cx-f->x/2,cz-f->z/2,f->x,f->z,2))putFeature(cx-f->x/2,cz-f->z/2,fire);
                }
                for(int placed=0,tries=0;placed<houses&&tries<houses*30;++tries) {
                    const int n=spec.buildings.n-(village?2:0);
                    const char* name=spec.buildings.p[splitmix(kitRng)%uint64_t(std::max(1,n))];
                    const auto* f=sizeOf(name);if(!f)continue;
                    int x,z;
                    if(village) {   // a loose ring around the fire
                        const auto [dx,dz]=kCompass[size_t(splitmix(kitRng)%8)];const int rad=10+int(splitmix(kitRng)%10);
                        x=cx+dx*rad/1000-f->x/2;z=cz+dz*rad/1000-f->z/2;
                    } else {
                        x=x0+int(splitmix(kitRng)%uint64_t(std::max(1,x1-x0-f->x)));z=z0+int(splitmix(kitRng)%uint64_t(std::max(1,z1-z0-f->z)));
                    }
                    if(!paved(x,z,f->x,f->z,village?1:3))continue;
                    putFeature(x,z,name);++placed;
                }
                // Nothing else is placed in a town: no mana, scenery or hills.
                for(int z=z0;z<z1;++z)for(int x=x0;x<x1;++x) {
                    const size_t i=size_t(z)*w+x;
                    if(village){const int dx=x-cx,dz=z-cz;if(dx*dx+dz*dz>24*24)continue;kitCell[i]=2;}
                    claim[i]=1;
                }
                ++r.towns;break;
            }
        }
    }
    // Authored relief: copy artwork AND its matching heights. Only self-contained
    // low-ground patches with near-level edges are admitted. No invisible mesas.
    std::vector<const tnt::Map*> hills;
    if(p.reliefDensity && p.layout!=Maze && p.layout!=Highlands && worldHasHills(p.mapType)) {
        auto paths=vfs.list(kReliefDir[p.mapType]);std::sort(paths.begin(),paths.end());
        for(const auto& path:paths) {
            const auto key=hpi::MountSet::key(path);
            if(!key.ends_with(".tnt"))continue;
            {
                // Audited retail surface families: matching height alone does
                // not make Veruna grass breakers fit sand, or Taros cobbles fit
                // low ground. These kits have no surrounding transition here.
                if(p.mapType==Veruna && key.find("/breaker")!=std::string::npos)continue;
                if(p.mapType==Taros && (key.ends_with("/unique23.tnt") ||
                    key.ends_with("/unique25.tnt") || key.ends_with("/unique27.tnt")))continue;
            }
            const auto* piece=load(path);if(!piece||piece->width>64||piece->height>64)continue;
            bool good=true;int high=land;
            for(int z=0;z<piece->height;++z)for(int x=0;x<piece->width;++x) {
                int height=piece->heights[size_t(z)*piece->width+x];high=std::max(high,height);
                if(height<land-4)good=false;
                if((x<2||z<2||x>=piece->width-2||z>=piece->height-2)&&std::abs(height-land)>4)good=false;
            }
            if(good&&high>=land+16)hills.push_back(piece);
        }
    }
    std::vector<uint8_t> relief(cells);
    uint64_t terrainRng=p.seed^0x173842febaULL;
    for(int z=16;z<h-32;z+=32)for(int x=16;x<w-32;x+=32) {
        if(hills.empty()||splitmix(terrainRng)%512>=p.reliefDensity)continue;
        const auto& piece=*hills[splitmix(terrainRng)%hills.size()];
        bool good=x+piece.width<w&&z+piece.height<h;
        for(int dz=-4;good&&dz<piece.height+4;++dz)for(int dx=-4;dx<piece.width+4;++dx) {
            const int nx=x+dx,nz=z+dz;
            if(nx<0||nz<0||nx>=w||nz>=h){good=false;break;}
            const size_t i=size_t(nz)*w+nx;
            if(reserved[i]||relief[i]||claim[i]||kitCell[i]||m.heights[i]!=land||cellCase(nx/32,nz/32)!=0){good=false;break;}
        }
        if(!good)continue;
        stamp(piece,x,z);++r.reliefPatches;
        for(int dz=0;dz<piece.height;++dz)for(int dx=0;dx<piece.width;++dx)relief[size_t(z+dz)*w+x+dx]=1;
    }
    // Reserve actual buildable rectangles and approach lanes before scenery. All
    // players receive the SAME tiers at the SAME walking distances on flat land.
    const auto& palette=kWorldFeat[p.mapType];
    const std::array<Point,3> homeOffsets={{{-24,-24},{24,-24},{0,28}}};
    std::vector<Point> deposits;
    // Level, unclaimed ground at one height: the low land, or (top) a kit plateau top.
    const auto flatAt=[&](int x,int z,int fx,int fz,int level,bool top) {
        if(x<1||z<1||x+fx>=w||z+fz>=h)return false;
        for(int dz=0;dz<fz;++dz)for(int dx=0;dx<fx;++dx) {
            const size_t i=size_t(z+dz)*w+x+dx;
            if(m.heights[i]!=level||claim[i]||(top&&!kitTop[i]))return false;
        }
        return true;
    };
    const bool anyTop=std::find(kitTop.begin(),kitTop.end(),1)!=kitTop.end();
    const auto flat=[&](int x,int z,int fx,int fz) {
        return flatAt(x,z,fx,fz,land,false)||(anyTop&&flatAt(x,z,fx,fz,spec.high,true));
    };
    uint64_t featureRng=p.seed^0xbe72431851ULL;
    const auto decorateDeposit=[&](int x,int z) {
        // Decorative rings stay outside the reserved build footprint. The actual
        // TDF footprints, not nominal 3x3 guesses, decide whether an arc fits, on
        // the deposit's own level (a plateau deposit keeps its ruins on top).
        const int level=m.heights[size_t(z)*w+x];
        const auto arcFits=[&](int ax,int az,int fx,int fz){return flatAt(ax,az,fx,fz,level,level!=land);};
        const auto& spec=kRing[p.mapType];
        if(!spec.n)return;   // a world without standing stones leaves its deposits bare
        int count=0;
        bool occupiedSlots[8]={};
        for(int slot=0;slot<8&&count<3;++slot) {
            for(int j=0;j<spec.n;++j) {
                const auto& arc=spec.p[j];if(arc.slot!=spec.order[slot])continue;
                const auto* f=sizeOf(arc.name);if(!f)continue;
                int ax=x+arc.dx,az=z+arc.dz;
                if(ax<x+4&&ax+f->x>x-3&&az<z+4&&az+f->z>z-3)continue;
                bool clear=arcFits(ax,az,f->x,f->z);
                for(int dz=0;clear&&dz<f->z;++dz)for(int dx=0;dx<f->x;++dx)
                    if(reserved[size_t(az+dz)*w+ax+dx]&1){clear=false;break;}
                if(clear) {putFeature(ax,az,arc.name);++count;occupiedSlots[arc.slot]=true;}
                break;
            }
        }
        if (count < 3) {
            // Authored offsets often collide with the lodestone yard or a reserved
            // approach. Move the appropriate arc outward instead of leaving bare
            // mana. Keep its orientation and use the real footprint throughout.
            for (int radius=1; radius<=16 && count<3; ++radius) {
                for (int slot=0; slot<8 && count<3; ++slot) {
                    for (int j=0; j<spec.n && count<3; ++j) {
                        const auto& arc=spec.p[j];
                        if (arc.slot!=spec.order[slot] || occupiedSlots[arc.slot]) continue;
                        const auto* f=sizeOf(arc.name); if (!f) continue;
                        const int ax=x+arc.dx+(arc.dx<0?-radius:arc.dx>0?radius:0);
                        const int az=z+arc.dz+(arc.dz<0?-radius:arc.dz>0?radius:0);
                        if (ax<x+4 && ax+f->x>x-3 && az<z+4 && az+f->z>z-3) continue;
                        bool clear=arcFits(ax,az,f->x,f->z);
                        for (int dz=0; clear && dz<f->z; ++dz)
                            for (int dx=0; dx<f->x; ++dx)
                                if (reserved[size_t(az+dz)*w+ax+dx]&1) { clear=false; break; }
                        if (clear) { putFeature(ax,az,arc.name); ++count; occupiedSlots[arc.slot]=true; break; }
                    }
                }
            }
            if (count==0) throw NoManaRuins{};
        }
    };
    const auto deposit=[&](int x,int z,int tier) {
        if(!putFeature(x,z,palette.sacred[tier],true))
            throw std::runtime_error("Random map is missing its world's sacred stone definitions");
        deposits.emplace_back(x,z);
        decorateDeposit(x,z);
        mark(x,z,7,1);
    };
    for(auto s:r.starts)for(int tier=0;tier<3;++tier) {
        const Point d={s.first+homeOffsets[tier].first,s.second+homeOffsets[tier].second};
        if(!flat(d.first-5,d.second-5,12,12))throw std::runtime_error("Random map home resource area is obstructed");
        reserveRoute(s,d,5);deposit(d.first,d.second,tier);
    }
    sim::NavGrid::Limits groundLimits;groundLimits.maxSlope=16;groundLimits.maxWaterDepth=0;
    sim::NavGrid ground(m.heights,w,h,m.seaLevel,groundLimits);
    ground.setObstacles(&obstacles);
    // Plateau tops are judged by who builds there: builders and infantry (GROUND3).
    sim::NavGrid::Limits builderLimits;builderLimits.maxSlope=30;builderLimits.maxWaterDepth=0;
    sim::NavGrid builders(m.heights,w,h,m.seaLevel,builderLimits);
    builders.setObstacles(&obstacles);
    // Open expansion sites, separated from homes and from one another. They must
    // lie on a reachable component, rather than merely looking dry on the map.
    std::vector<uint8_t> reachable(cells);
    std::vector<std::vector<int>> homeDistances=distancesFrom(ground,r.starts,6);
    for(size_t k=0;k<r.starts.size();++k) {
        const auto& d=homeDistances[k];
        if(p.layout!=Islands&&k==0)for(auto s:r.starts)
            if(d[size_t(s.second)*w+s.first]<0)throw std::runtime_error("Random map has disconnected starts");
        for(size_t i=0;i<cells;++i)if(d[i]>=0)reachable[i]=1;
    }
    // Add expansions in complete rounds: each player gets the same number and
    // tier, and the route-length band grows equally. A cramped map stops a round
    // for everyone instead of silently favoring whichever start was scanned first.
    const int rounds=extraManaRounds(p.manaDensity);
    int highRounds=0;
    for(int round=0;round<rounds;++round) {
      bool roundDone=false,stopRounds=false;
      for(int pass=0;pass<2&&!roundDone&&!stopRounds;++pass) {
        std::vector<Point> sites;
        // From the second round on, a round first tries the kit plateau tops a ramp
        // reaches (every round until one succeeds, then every other), so high
        // ground is worth taking. It is all or nothing: each player needs a top site
        // in one common walking-distance band (through the ramps), nearer to it than
        // to any other start; otherwise (or if its ruins find no room) the round
        // falls back to the low ground.
        std::vector<std::vector<int>> topDistances;const bool highRound=pass==0;
        if(highRound&&!(anyTop && round>=1 && (!highRounds || round%2==1)))continue;
        if(highRound) {
            builders.markClearanceDirty();
            topDistances=distancesFrom(builders,r.starts,3);
            std::vector<std::vector<std::pair<int,Point>>> tops(r.starts.size());
            for(int z=8;z<h-8;z+=4)for(int x=8;x<w-8;x+=4) {
                const size_t i=size_t(z)*w+x;
                if(!kitTop[i]||reserved[i]||!flatAt(x-6,z-6,14,14,spec.high,true))continue;
                for(size_t player=0;player<r.starts.size();++player) {
                    const int distance=topDistances[player][i];if(distance<0)continue;
                    bool nearest=true;
                    for(size_t other=0;other<topDistances.size();++other)
                        if(other!=player&&topDistances[other][i]>=0&&topDistances[other][i]<distance+8)nearest=false;
                    if(nearest)tops[player].push_back({distance,{x,z}});
                }
            }
            const int target=64+round*24;
            for(int offset=0;offset<=480&&sites.empty();offset+=4)for(int sign:{1,-1}) {
                const int band=target+sign*offset;if(band<64||(offset==0&&sign<0))continue;
                std::vector<Point> chosen;
                for(size_t player=0;player<r.starts.size();++player) {
                    Point best={-1,-1};int bestScore=13;
                    for(const auto& [distance,at]:tops[player]) {
                        const int score=std::abs(distance-band);if(score>=bestScore)continue;
                        bool spaced=true;
                        for(const auto* group:{&deposits,&chosen})for(auto pt:*group)
                            if((pt.first-at.first)*(pt.first-at.first)+(pt.second-at.second)*(pt.second-at.second)<32*32)spaced=false;
                        if(spaced){best=at;bestScore=score;}
                    }
                    if(best.first<0)break;
                    chosen.push_back(best);
                }
                if(chosen.size()==r.starts.size()){sites=chosen;break;}
            }
        }
        if(!highRound)
        for(size_t player=0;player<r.starts.size();++player) {
            Point best={-1,-1};int bestScore=1000000;
            const auto& d=homeDistances[player];
            const int target=64+round*24;
            // Walking distance is at least the cardinal distance. Sampling the
            // whole map made these short bands practically disappear on 64x64
            // maps; use their containing box around this owner's home instead.
            const auto home=r.starts[player];const int reach=target+12;
            const int x0=std::max(8,home.first-reach),x1=std::min(w-8,home.first+reach+1);
            const int z0=std::max(8,home.second-reach),z1=std::min(h-8,home.second+reach+1);
            for(int attempt=0;attempt<800;++attempt) {
                const int x=x0+int(splitmix(featureRng)%uint64_t(x1-x0));
                const int z=z0+int(splitmix(featureRng)%uint64_t(z1-z0));
                const size_t i=size_t(z)*w+x;const int distance=d[i];
                if(distance<target-12||distance>target+12||reserved[i]||!flatAt(x-6,z-6,14,14,land,false))continue;
                bool good=true;
                for(size_t other=0;other<homeDistances.size();++other)
                    if(other!=player&&homeDistances[other][i]>=0&&homeDistances[other][i]<distance+8)good=false;
                for(const auto* group:{&deposits,&sites})for(auto pt:*group)
                    if((pt.first-x)*(pt.first-x)+(pt.second-z)*(pt.second-z)<32*32)good=false;
                if(!good)continue;
                const int score=std::abs(distance-target);
                if(score<bestScore){best={x,z};bestScore=score;}
            }
            if(best.first<0)break;
            sites.push_back(best);
        }
        if(sites.size()!=r.starts.size()){stopRounds=!highRound;continue;}
        const auto oldDeposits=deposits.size(),oldNames=m.featureNames.size();
        undo.clear();transaction=true;
        // Reserve every route in this round before its ruins are placed.
        for(size_t player=0;player<sites.size();++player) {
            const auto site=sites[player];
            // Follow the existing footprint-valid distance field back home, and
            // reserve this approach before adding any trees or rocks.
            Point at=site;const auto& d=highRound?topDistances[player]:homeDistances[player];
            while(d[size_t(at.second)*w+at.first]>0) {
                mark(at.first,at.second,4,5);
                const int here=d[size_t(at.second)*w+at.first];
                for(auto [dx,dz]:std::array<Point,4>{{{0,-1},{1,0},{0,1},{-1,0}}}) {
                    const int nx=at.first+dx,nz=at.second+dz;
                    if(nx>=0&&nz>=0&&nx<w&&nz<h&&d[size_t(nz)*w+nx]==here-1){at={nx,nz};break;}
                }
            }
        }
        try {
            for (auto site : sites) deposit(site.first,site.second,1);
        } catch(const NoManaRuins&) {
            // Ruins can lose their last free position when all approaches in
            // the round are reserved. Omit the complete round fairly, restoring
            // only changed cells instead of copying the entire map's planes.
            for(auto it=undo.rbegin();it!=undo.rend();++it) {
                m.features[it->index]=it->feature;reserved[it->index]=it->reserved;
                claim[it->index]=it->claim;obstacles[it->index]=it->obstacle;
            }
            deposits.resize(oldDeposits);m.featureNames.resize(oldNames);
            transaction=false;undo.clear();stopRounds=!highRound;continue;
        }
        transaction=false;undo.clear();roundDone=true;highRounds+=highRound;
      }
      if(stopRounds)break;
        // New ruins are real obstacles. Plan the next round against the
        // updated terrain, rather than routing through an earlier ruin.
        ground.markClearanceDirty();
        homeDistances=distancesFrom(ground,r.starts,6);
    }
    const size_t roundDeposits=deposits.size();
    // Industrial Zone: the pad at the end of every dead-end walkway holds a sacred
    // stone, in the shipped tier mix (223:446:537). They are reached along the
    // walkways, and the final validation checks so, as for scattered mana. Each
    // cut-off pad holds one too, after them: only flying builders reach those, so
    // the validation leaves them out.
    size_t padBegin=0,padEnd=0;
    if(!izDeadEnds.empty()||!izPads.empty()) {
        uint64_t padRng=p.seed^0x70616473ULL;
        const size_t deadEnds=izDeadEnds.size();
        izDeadEnds.insert(izDeadEnds.end(),izPads.begin(),izPads.end());
        for(size_t k=0;k<izDeadEnds.size();++k) {
            const auto [sx,sz]=izDeadEnds[k];
            if(k==deadEnds)padBegin=deposits.size();
            // The pad's middle: the centre of its largest square of plating (a dead
            // end's walkway is narrower), averaged over every square that size. The
            // shipped pads sit off their sections' centres. In half cells.
            int side[32][32]={},best2=0,cx2=0,cz2=0,count=0;
            for(int dz=0;dz<32;++dz)for(int dx=0;dx<32;++dx) {
                const size_t i=size_t(sz*32+dz)*w+sx*32+dx;
                if(m.features[i]==0xfffc)continue;
                side[dz][dx]=1+(dz&&dx?std::min({side[dz-1][dx],side[dz][dx-1],side[dz-1][dx-1]}):0);
                const int n=side[dz][dx];
                if(n>best2){best2=n;cx2=cz2=count=0;}
                if(n==best2){cx2+=2*dx+2-n;cz2+=2*dz+2-n;++count;}
            }
            if(!count)continue;
            cx2=2*sx*32+cx2/count;cz2=2*sz*32+cz2/count;
            Point best={-1,-1};int bestScore=1<<30;
            for(int z=sz*32+4;z<sz*32+28;++z)for(int x=sx*32+4;x<sx*32+28;++x) {
                // The 2x2 stone and a cell round it on open pad, within ±4 of the
                // plating (a lodestone, 2x2 too, takes a height spread of 10).
                // The pads' raised trim stays out of the way without pushing the stone
                // off the middle.
                bool level=true;
                for(int dz=-1;level&&dz<3;++dz)for(int dx=-1;dx<3;++dx) {
                    const size_t i=size_t(z+dz)*w+x+dx;
                    if(std::abs(int(m.heights[i])-land)>4||claim[i]||m.features[i]==0xfffc){level=false;break;}
                }
                const int score=(2*x+2-cx2)*(2*x+2-cx2)+(2*z+2-cz2)*(2*z+2-cz2);   // the stone's middle
                if(level&&score<bestScore){best={x,z};bestScore=score;}
            }
            if(best.first<0)continue;
            const uint32_t roll=uint32_t(splitmix(padRng)%1206);
            deposit(best.first,best.second,roll<223?0:roll<669?1:2);
            ++(k<deadEnds?r.scatteredMana:r.padMana);
        }
        if(deadEnds==izDeadEnds.size())padBegin=deposits.size();
        padEnd=deposits.size();
    }
    // Scattered neutral mana: a jittered grid at the level's pitch (one
    // candidate per pitch^2 cells, so the count follows map area), on flat open
    // ground away from every home economy and other deposits. Its own RNG
    // stream: the scenery draws after it are unchanged. Each site's approach is
    // reserved along the nearest home's walking-distance field until it joins an
    // existing lane, so later scenery cannot wall it in.
    if(const int pitch=scatterPitch(p.manaScatter)) {
        const int nx=std::max(1,w/pitch),nz=std::max(1,h/pitch);
        std::vector<Point> chosen(size_t(nx)*nz,{-1,-1});
        std::vector<Point> sites;
        uint64_t scatterRng=p.seed^0x6e65757472616cULL;
        for(int gz=0;gz<nz;++gz)for(int gx=0;gx<nx;++gx) {
            const int x0=std::max(8,gx*w/nx),x1=std::min(w-9,(gx+1)*w/nx);
            const int z0=std::max(8,gz*h/nz),z1=std::min(h-9,(gz+1)*h/nz);
            if(x1<=x0||z1<=z0)continue;
            for(int attempt=0;attempt<8;++attempt) {
                const int x=x0+int(splitmix(scatterRng)%uint64_t(x1-x0));
                const int z=z0+int(splitmix(scatterRng)%uint64_t(z1-z0));
                const size_t i=size_t(z)*w+x;
                if(reserved[i]||!reachable[i]||!flat(x-6,z-6,14,14))continue;
                bool good=true;
                for(auto s:r.starts)
                    if((s.first-x)*(s.first-x)+(s.second-z)*(s.second-z)<60*60)good=false;
                for(auto pt:deposits)
                    if((pt.first-x)*(pt.first-x)+(pt.second-z)*(pt.second-z)<24*24)good=false;
                for(int dz=-1;good&&dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
                    if(gx+dx<0||gz+dz<0||gx+dx>=nx||gz+dz>=nz)continue;
                    const auto pt=chosen[size_t(gz+dz)*nx+gx+dx];
                    if(pt.first>=0&&(pt.first-x)*(pt.first-x)+(pt.second-z)*(pt.second-z)<24*24)good=false;
                }
                if(!good)continue;
                chosen[size_t(gz)*nx+gx]={x,z};sites.emplace_back(x,z);break;
            }
        }
        // Reserve every approach before any ruins exist, as the expansion rounds do.
        for(auto site:sites) {
            size_t owner=0;int best=-1;
            for(size_t player=0;player<homeDistances.size();++player) {
                const int d=homeDistances[player][size_t(site.second)*w+site.first];
                if(d>=0&&(best<0||d<best)){best=d;owner=player;}
            }
            // Walk first, mark after: the lane's own marks must not end the walk.
            Point at=site;const auto& d=homeDistances[owner];std::vector<Point> lane;
            while(d[size_t(at.second)*w+at.first]>0&&!(reserved[size_t(at.second)*w+at.first]&4)) {
                lane.push_back(at);
                const int here=d[size_t(at.second)*w+at.first];
                for(auto [dx,dz]:std::array<Point,4>{{{0,-1},{1,0},{0,1},{-1,0}}}) {
                    const int nx2=at.first+dx,nz2=at.second+dz;
                    if(nx2>=0&&nz2>=0&&nx2<w&&nz2<h&&d[size_t(nz2)*w+nx2]==here-1){at={nx2,nz2};break;}
                }
            }
            for(auto pt:lane)mark(pt.first,pt.second,4,5);
        }
        for(auto site:sites) {
            // Shipped sacred-stone tier mix (223:446:537), as retail maps scatter them.
            const uint32_t roll=uint32_t(splitmix(scatterRng)%1206);
            const int tier=roll<223?0:roll<669?1:2;
            if(!flat(site.first-6,site.second-6,14,14))continue;   // an earlier ring took it
            const auto oldDeposits=deposits.size(),oldNames=m.featureNames.size();
            undo.clear();transaction=true;
            try {deposit(site.first,site.second,tier);++r.scatteredMana;}
            catch(const NoManaRuins&) {
                for(auto it=undo.rbegin();it!=undo.rend();++it) {
                    m.features[it->index]=it->feature;reserved[it->index]=it->reserved;
                    claim[it->index]=it->claim;obstacles[it->index]=it->obstacle;
                }
                deposits.resize(oldDeposits);m.featureNames.resize(oldNames);
            }
            transaction=false;undo.clear();
        }
    }
    // A separate noise field makes forest stands and rocky regions, with clear
    // paths through them. Dry, flat approaches around bases are never filled in.
    for(int z=1;z<h-4;++z)for(int x=1;x<w-4;++x) {
        const size_t i=size_t(z)*w+x;
        if(reserved[i]&1||claim[i]||!reachable[i])continue;
        const uint64_t roll=splitmix(featureRng);
        const int forest=int(noise(p.seed^0x715921,x,z,24)) + (p.layout==Jungle?60:0);
        const int rocks=int(noise(p.seed^0x359742,x,z,18));
        const char* name=nullptr;
        if(forest>125&&palette.trees.n&&int(roll%10000)<int(p.treeDensity)*(forest-110)*4/160)
            name=palette.trees.p[(roll>>24)%palette.trees.n];
        else if(rocks>145&&int((roll>>12)%10000)<int(p.rockDensity)*(rocks-130)*3/160)
            name=palette.rocks.p[std::min((roll>>24)%palette.rocks.n,(roll>>34)%palette.rocks.n)];
        // The volcano's "rocks" are whole buildings: one in six, near takx20's density.
        if(name&&lavaWorld(p.mapType)&&(roll>>44)%6)continue;
        if(!name)continue;
        const auto* f=sizeOf(name);if(!f||!flat(x-1,z-1,f->x+2,f->z+2))continue;
        bool good=true;
        for(int dz=-1;dz<=f->z;++dz)for(int dx=-1;dx<=f->x;++dx)
            if(reserved[size_t(z+dz)*w+x+dx]&1)good=false;
        if(good)putFeature(x,z,name);
    }
    // Kit plateau tops get their own forests and rocks from the same fields, on
    // their own random stream; a ramp-less top is scenery only.
    uint64_t topRng=p.seed^0x746f707363656e65ULL;
    for(int z=1;z<h-4;++z)for(int x=1;x<w-4;++x) {
        const size_t i=size_t(z)*w+x;
        if(!kitTop[i]||claim[i]||(reserved[i]&1))continue;
        const uint64_t roll=splitmix(topRng);
        const int forest=int(noise(p.seed^0x715921,x,z,24))+(p.layout==Jungle?60:0);
        const int rocks=int(noise(p.seed^0x359742,x,z,18));
        const char* name=nullptr;
        if(forest>125&&palette.trees.n&&int(roll%10000)<int(p.treeDensity)*(forest-110)*4/160)
            name=palette.trees.p[(roll>>24)%palette.trees.n];
        else if(rocks>145&&int((roll>>12)%10000)<int(p.rockDensity)*(rocks-130)*3/160)
            name=palette.rocks.p[std::min((roll>>24)%palette.rocks.n,(roll>>34)%palette.rocks.n)];
        // The volcano's "rocks" are whole buildings: one in six, near takx20's density.
        if(name&&lavaWorld(p.mapType)&&(roll>>44)%6)continue;
        if(!name)continue;
        const auto* f=sizeOf(name);if(!f)continue;
        bool good=true;
        // A footprint that runs off the bottom of the map is refused: its cells past the
        // last row do not exist (one past the right edge reads the next row's, as ever).
        for(int dz=-1;good&&dz<=f->z;++dz)for(int dx=-1;dx<=f->x;++dx) {
            const size_t j=size_t(z+dz)*w+x+dx;
            if(j>=kitTop.size()||!kitTop[j]||claim[j]||m.heights[j]!=spec.high){good=false;break;}
        }
        if(good)putFeature(x,z,name);
    }
    if(anyTop) {
        std::vector<uint8_t> holding(size_t(std::max(1,r.plateaus)),0);
        for(auto [x,z]:deposits) {
            if(!kitTop[size_t(z)*w+x])continue;
            ++r.plateauMana;
            const int c=component[size_t(z/32)*(sw+1)+x/32];
            if(c>=0&&c<int(holding.size())&&!holding[size_t(c)]){holding[size_t(c)]=1;++r.manaPlateaus;}
        }
    }
    // Final footprint-aware validation sees scenery exactly as the simulation
    // will. Failure is explicit; an invalid map is never silently launched.
    ground.markClearanceDirty();
    std::vector<uint8_t> scatterReached(deposits.size()-roundDeposits);
    for(auto s:r.starts)
        if(!flat(s.first-12,s.second-12,24,24))throw std::runtime_error("Random map has an obstructed base");
    homeDistances=distancesFrom(ground,r.starts,6);
    for(size_t k=0;k<r.starts.size();++k) {
        const auto s=r.starts[k];
        const auto& d=homeDistances[k];
        for(size_t j=k*3;j<k*3+3;++j)
            if(d[size_t(deposits[j].second)*w+deposits[j].first]<0)
                throw std::runtime_error("Random map home mana is unreachable");
        std::vector<int> onFoot;   // plateau deposits: builders and infantry
        for(size_t j=r.starts.size()*3+k;j<roundDeposits;j+=r.starts.size()) {
            const size_t i=size_t(deposits[j].second)*w+deposits[j].first;
            if(d[i]>=0)continue;
            if(kitTop[i]) {
                if(onFoot.empty()){builders.markClearanceDirty();onFoot=distances(builders,s,3);}
                if(onFoot[i]>=0)continue;
            }
            throw std::runtime_error("Random map expansion mana is unreachable");
        }
        if(p.layout!=Islands)for(auto target:r.starts)
            if(d[size_t(target.second)*w+target.first]<0)throw std::runtime_error("Random map scenery blocks an army route");
        for(size_t j=roundDeposits;j<deposits.size();++j)
            if((j>=padBegin&&j<padEnd)||d[size_t(deposits[j].second)*w+deposits[j].first]>=0)scatterReached[j-roundDeposits]=1;
    }
    if(std::find(scatterReached.begin(),scatterReached.end(),0)!=scatterReached.end())
        throw std::runtime_error("Random map scattered mana is unreachable");
    if(p.layout==Islands || p.layout==Ports) {
        sim::NavGrid::Limits shipLimits;shipLimits.minWaterDepth=15;
        sim::NavGrid ships(m.heights,w,h,m.seaLevel,shipLimits);ships.setObstacles(&obstacles);
        std::vector<int> waterDistances;
        for(auto s:r.starts) {
            Point best={-1,-1};int bestDistance=w*w+h*h;
            for(int z=16;z<h-16;++z)for(int x=16;x<w-16;++x) {
                const int d=(x-s.first)*(x-s.first)+(z-s.second)*(z-s.second);
                if(d>=bestDistance||!ships.fits(x,z,15))continue;
                // Sea Fort's yard is 6x18, and output ships need clear water
                // beyond it. The NavGrid clearance cache saturates at 15, so use
                // its exact cell classifier for this larger construction area.
                bool open=true;
                for(int dz=-24;open&&dz<24;++dz)for(int dx=-12;dx<12;++dx)
                    if(!ships.walkable(x+dx,z+dz)){open=false;break;}
                if(!open)continue;
                if(!waterDistances.empty()&&waterDistances[size_t(z)*w+x]<0)continue;
                best={x,z};bestDistance=d;
            }
            if(best.first<0||bestDistance>(p.layout==Ports?std::max(w,h)*std::max(w,h):(islandRadius+96)*(islandRadius+96)))throw std::runtime_error("Random island has no usable connected harbor: start="+std::to_string(s.first)+","+std::to_string(s.second)+" best="+std::to_string(best.first)+","+std::to_string(best.second)+" d2="+std::to_string(bestDistance));
            if(waterDistances.empty())waterDistances=distances(ships,best,8);
            r.harbors.push_back(best);
        }
    }
    const auto water=std::count_if(m.heights.begin(),m.heights.end(),[&](int height){return height<m.seaLevel;});
    r.waterPercent=int(water*100/cells);
    return r;
}
}

Result generateBalanced(const Params& p,const hpi::Vfs& vfs) {
    // Kit terrain can leave too little room for a fair map: a plateau where an
    // expansion round needed space, or a range across the only way to a deposit.
    // Retry with half the kit amounts, then none. Still deterministic, and the
    // last attempt is exactly the terrain without kits.
    Params attempt=p;
    for(int tries=0;;++tries) {
        try {Result r=generateOnce(attempt,vfs);r.kitFallback=tries;return r;}
        catch(const std::runtime_error&) {
            if(!attempt.plateauDensity&&!attempt.mountainDensity&&!attempt.townDensity)throw;
            if(tries==0) {attempt.plateauDensity/=2;attempt.mountainDensity/=2;attempt.townDensity/=2;}
            else attempt.plateauDensity=attempt.mountainDensity=attempt.townDensity=0;
        }
    }
}
} // namespace tak::mapgen
