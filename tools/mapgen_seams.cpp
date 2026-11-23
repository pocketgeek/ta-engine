// Seam hunt for the random map generator: finds terrain tiles that should not sit
// next to each other. Two independent signals, so neither alone decides:
//
//  (a) Retail adjacency whitelist. Every shipped map (multiplayer and campaign) is
//      tiled from terrain JPGs; at each boundary between two different JPGs the
//      pair (key A, key B, direction) is recorded. A generated boundary whose pair
//      never occurs in retail is flagged. A JPG meeting another copy of itself out
//      of step (column 15 beside column 0, a piece beside itself) is a boundary too;
//      only tiles in their authored order inside one JPG are not judged.
//  (b) Pixels. Across each tile boundary the colour step is compared with the step
//      just inside each tile (edge score), and the mean tone of the 4-pixel strips
//      either side with the strip-to-strip change inside each tile (tone score), so
//      busy textures do not false-alarm.
//
// A candidate is a boundary segment flagged by (a), ranked by (b); strong (b)-only
// segments are listed separately (height or texture issues the whitelist allows).
//
//   mapgen_seams <data> retail            whitelist size, and (b) on retail itself
//                                         (leave-one-map-out (a) as a sanity check)
//   mapgen_seams <data> pairs              every straight hillside x cliff join, both
//                                         sides, scored from the section art
//   mapgen_seams <data> sweep <shard> <shards> [quick]
//                                         generate the sweep's maps (deterministic
//                                         job list), print HIT lines; quick is a
//                                         smaller sweep that fails on any candidate
//                                         or strong seam (the mapgen_seams test)
#include "hpi/hpi.h"
#include "tnt/mapgen.h"
#include "tnt/mapgen_internal.h"
#include "tnt/tnt.h"
#include "util/jpeg.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace tak;

// Per 32px tile, per edge (0 N, 1 S, 2 W, 3 E): the outermost pixel line, the mean
// step from it to the next line in, and the mean colour of the outer and the next
// 4-pixel strips.
struct Edge { std::array<std::array<uint8_t,3>,32> line{}; float inner=0; std::array<float,3> s0{},s1{}; };
struct Tile { std::array<Edge,4> e; };

class Tiles {
public:
    explicit Tiles(const hpi::Vfs& vfs) : vfs_(vfs) {}
    const Tile* get(uint32_t key,int col,int row) {
        auto it=cache_.find(key);
        if(it==cache_.end()) it=cache_.emplace(key,decode(key)).first;
        const auto& v=it->second;
        if(v.grid==0||col>=v.grid||row>=v.grid)return nullptr;
        return &v.tiles[size_t(row)*v.grid+col];
    }
private:
    struct Image { int grid=0; std::vector<Tile> tiles; };
    Image decode(uint32_t key) {
        Image out;
        char name[32];std::snprintf(name,sizeof name,"terrain/%08x.jpg",key);
        if(!vfs_.has(name))return out;
        jpeg::Image img;
        try{img=jpeg::load(vfs_.read(name));}catch(...){return out;}
        if(img.width<32||img.height<32)return out;
        out.grid=std::min(img.width,img.height)/32;
        out.tiles.resize(size_t(out.grid)*out.grid);
        auto px=[&](int x,int y,int c){return int(img.rgba[(size_t(y)*img.width+x)*4+c]);};
        for(int r=0;r<out.grid;++r)for(int c=0;c<out.grid;++c) {
            Tile& t=out.tiles[size_t(r)*out.grid+c];
            for(int e=0;e<4;++e) {
                // (i, d): i runs along the edge, d is the depth into the tile.
                auto at=[&](int i,int d,int ch){
                    int x,y;
                    switch(e){case 0:x=i;y=d;break;case 1:x=i;y=31-d;break;case 2:x=d;y=i;break;default:x=31-d;y=i;}
                    return px(c*32+x,r*32+y,ch);};
                float inner=0;
                for(int i=0;i<32;++i)for(int ch=0;ch<3;++ch) {
                    t.e[e].line[i][ch]=uint8_t(at(i,0,ch));
                    inner+=std::abs(at(i,0,ch)-at(i,1,ch));
                }
                t.e[e].inner=inner/96.f;
                for(int ch=0;ch<3;++ch) {
                    float a=0,b=0;
                    for(int i=0;i<32;++i)for(int d=0;d<4;++d){a+=at(i,d,ch);b+=at(i,d+4,ch);}
                    t.e[e].s0[ch]=a/128.f;t.e[e].s1[ch]=b/128.f;
                }
            }
        }
        return out;
    }
    const hpi::Vfs& vfs_;
    std::map<uint32_t,Image> cache_;
};

struct Score { float edge=0,tone=0; };
// Boundary between tile A and its neighbour B; dir 0 = B east of A, 1 = B south.
Score score(const Tile& a,const Tile& b,int dir) {
    const Edge& ea=a.e[dir==0?3:1];
    const Edge& eb=b.e[dir==0?2:0];
    float cross=0;
    for(int i=0;i<32;++i)for(int ch=0;ch<3;++ch)cross+=std::abs(int(ea.line[i][ch])-int(eb.line[i][ch]));
    cross/=96.f;
    Score s;
    s.edge=cross/((ea.inner+eb.inner)/2+4);
    float toneStep=0,innerA=0,innerB=0;
    for(int ch=0;ch<3;++ch){toneStep+=std::abs(ea.s0[ch]-eb.s0[ch]);innerA+=std::abs(ea.s0[ch]-ea.s1[ch]);innerB+=std::abs(eb.s0[ch]-eb.s1[ch]);}
    s.tone=(toneStep/3)/((innerA+innerB)/6+3);
    return s;
}

using Pair=std::tuple<uint32_t,uint32_t,int>;   // key A, key B, direction (0 E, 1 S)

// Tiles a and b (b east of a for dir 0, south for 1) meet at a boundary unless they
// are neighbours in the same JPG's authored order.
bool boundary(const tnt::Map& m,size_t a,size_t b,int dir) {
    if(m.tileKeys[a]!=m.tileKeys[b])return true;
    return dir==0?(m.tileCols[b]!=m.tileCols[a]+1||m.tileRows[b]!=m.tileRows[a])
                 :(m.tileRows[b]!=m.tileRows[a]+1||m.tileCols[b]!=m.tileCols[a]);
}

// Every cross-JPG boundary of a map: a segment is a run of tiles along one boundary
// line with the same key pair.
struct Segment { Pair pair; int bx=0,bz=0,length=0; float edge=0,tone=0; };
std::vector<Segment> segments(const tnt::Map& m,Tiles& tiles) {
    std::vector<Segment> out;
    for(int dir=0;dir<2;++dir)
    for(int bz=0;bz<m.blocksY-(dir==1);++bz)for(int bx=0;bx<m.blocksX-(dir==0);++bx) {
        const size_t a=size_t(bz)*m.blocksX+bx,b=dir==0?a+1:a+m.blocksX;
        if(!boundary(m,a,b,dir))continue;
        const Pair pair{m.tileKeys[a],m.tileKeys[b],dir};
        Score s{};
        const Tile* ta=tiles.get(m.tileKeys[a],m.tileCols[a],m.tileRows[a]);
        const Tile* tb=tiles.get(m.tileKeys[b],m.tileCols[b],m.tileRows[b]);
        if(ta&&tb)s=score(*ta,*tb,dir);
        // Extend the previous segment when it continues along the same line.
        if(!out.empty()) {
            auto& last=out.back();
            const bool along=dir==0?(last.bx==bx&&last.bz+last.length==bz):(last.bz==bz&&last.bx+last.length==bx);
            if(last.pair==pair&&along){last.edge+=s.edge;last.tone+=s.tone;++last.length;continue;}
        }
        out.push_back({pair,bx,bz,1,s.edge,s.tone});
    }
    // Runs along a column (dir 0) are found row by row above, so they come out one
    // tile long; merge them along z here.
    std::vector<Segment> merged;
    std::map<std::tuple<uint32_t,uint32_t,int,int>,size_t> open;   // pair + line -> index
    std::sort(out.begin(),out.end(),[](const Segment& l,const Segment& r){
        return std::get<2>(l.pair)!=std::get<2>(r.pair)?std::get<2>(l.pair)<std::get<2>(r.pair):
               std::get<2>(l.pair)==0?(l.bx!=r.bx?l.bx<r.bx:l.bz<r.bz):(l.bz!=r.bz?l.bz<r.bz:l.bx<r.bx);});
    for(const auto& s:out) {
        if(!merged.empty()) {
            auto& last=merged.back();
            const int dir=std::get<2>(s.pair);
            const bool along=dir==0?(last.bx==s.bx&&last.bz+last.length==s.bz):(last.bz==s.bz&&last.bx+last.length==s.bx);
            if(last.pair==s.pair&&along){last.edge+=s.edge;last.tone+=s.tone;last.length+=s.length;continue;}
        }
        merged.push_back(s);
    }
    for(auto& s:merged){s.edge/=float(s.length);s.tone/=float(s.length);}
    return merged;
}

std::vector<std::string> retailMaps(const hpi::Vfs& vfs) {
    std::vector<std::string> out;
    for(const char* dir:{"maps/","missions/"})
        for(const auto& p:vfs.list(dir))if(hpi::MountSet::key(p).ends_with(".tnt"))out.push_back(p);
    std::sort(out.begin(),out.end());
    out.erase(std::unique(out.begin(),out.end()),out.end());
    return out;
}

std::map<uint32_t,std::string> sectionNames(const hpi::Vfs& vfs) {
    std::map<uint32_t,std::string> names;
    std::vector<std::string> paths=vfs.list("sections/");
    std::sort(paths.begin(),paths.end());
    for(const auto& p:paths) {
        const auto k=hpi::MountSet::key(p);if(!k.ends_with(".tnt"))continue;
        try{const auto m=tnt::Map::load(vfs.read(p),p);
            for(auto key:m.tileKeys)names.emplace(key,k.substr(9));}catch(...){}
    }
    return names;
}
std::string nameOf(const std::map<uint32_t,std::string>& names,uint32_t key) {
    auto it=names.find(key);if(it!=names.end())return it->second;
    char b[16];std::snprintf(b,sizeof b,"key:%08x",key);return b;
}

// Thresholds for (b), from the retail calibration: a segment is "strong" above them.
constexpr float kEdge=3.0f,kTone=3.0f;

struct Whitelist { std::map<Pair,int> maps; };   // pair -> number of retail maps using it
Whitelist buildWhitelist(const hpi::Vfs& vfs,const std::vector<std::string>& paths,Tiles* tiles,
                         std::vector<std::pair<std::string,std::vector<Segment>>>* keep) {
    Whitelist w;
    for(const auto& p:paths) {
        tnt::Map m;try{m=tnt::Map::load(vfs.read(p),p);}catch(...){continue;}
        std::set<Pair> seen;
        for(int dir=0;dir<2;++dir)
        for(int bz=0;bz<m.blocksY-(dir==1);++bz)for(int bx=0;bx<m.blocksX-(dir==0);++bx) {
            const size_t a=size_t(bz)*m.blocksX+bx,b=dir==0?a+1:a+m.blocksX;
            if(boundary(m,a,b,dir))seen.insert({m.tileKeys[a],m.tileKeys[b],dir});
        }
        for(const auto& pair:seen)++w.maps[pair];
        if(keep&&tiles)keep->push_back({p,segments(m,*tiles)});
    }
    return w;
}

struct Job { mapgen::Params p; std::string label; };
std::vector<Job> sweepJobs(bool quick) {
    using namespace mapgen;
    std::vector<Job> jobs;
    // Slider sets: default, all max, and mixed extremes of Plateaus/Mountains/Towns/
    // Hills/Water.
    struct Set { const char* name; int plateau,mountain,town,hills,water; };
    const Set sets[]={{"default",-1,-1,-1,-1,-1},{"max",255,255,255,255,255},
                      {"plateaus",255,0,255,0,255},{"mountains",0,255,0,255,0},{"towns",128,128,255,255,0}};
    const int sizes[]={16,24,32,40,48,56,64};
    const int players[]={2,4,8};
    for(int world=0;world<kMapTypes;++world)
    for(int layout:{int(Mainland),int(Lakes),int(Islands),int(themedLayout(uint8_t(world)))})
    for(int size:sizes)for(int pl:players)for(const auto& set:sets)
    for(int seed=1;seed<=(quick?1:3);++seed) {
        if(quick&&(size>32||size%16||pl==4))continue;
        Params p;p.mapType=uint8_t(world);p.layout=uint8_t(layout);p.players=uint8_t(pl);
        p.widthCells=p.heightCells=uint16_t(size*32);p.seed=uint64_t(seed*7919+size*31+pl);
        if(set.plateau>=0){p.plateauDensity=uint8_t(set.plateau);p.mountainDensity=uint8_t(set.mountain);
            p.townDensity=uint8_t(set.town);p.reliefDensity=uint8_t(set.hills);p.waterDensity=uint8_t(set.water);}
        p=sanitize(p);
        if(p.widthCells!=size*32)continue;   // below the layout's minimum: skip, not resize
        jobs.push_back({p,set.name});
    }
    return jobs;
}
}  // namespace

int main(int argc,char** argv) {
    if(argc<3){std::fprintf(stderr,"usage: mapgen_seams <data> retail | sweep <shard> <shards> [quick]\n");return 2;}
    const auto vfs=hpi::mountRetailRoot(argv[1]);
    const std::string mode=argv[2];
    Tiles tiles(vfs);
    const auto names=sectionNames(vfs);
    const auto paths=retailMaps(vfs);
    if(mode=="retail") {
        std::vector<std::pair<std::string,std::vector<Segment>>> kept;
        const auto w=buildWhitelist(vfs,paths,&tiles,&kept);
        std::printf("retail maps %zu, whitelisted pairs %zu\n",paths.size(),w.maps.size());
        // Calibration: the (b) distribution over retail's own boundaries, and the
        // leave-one-map-out (a)+(b) candidates (pairs only that map uses AND strong).
        std::vector<float> edges,tones;size_t segs=0,strong=0,unique=0,candidates=0;
        for(const auto& [path,ss]:kept)for(const auto& s:ss) {
            ++segs;edges.push_back(s.edge);tones.push_back(s.tone);
            const bool hot=s.edge>kEdge||s.tone>kTone;
            const bool alone=w.maps.at(s.pair)<=1;   // no other retail map has the pair
            strong+=hot;unique+=alone;
            if(hot&&alone){++candidates;
                std::printf("RETAIL-CANDIDATE edge=%.2f tone=%.2f len=%d %s | %s dir=%d in %s at %d,%d\n",s.edge,s.tone,s.length,
                    nameOf(names,std::get<0>(s.pair)).c_str(),nameOf(names,std::get<1>(s.pair)).c_str(),std::get<2>(s.pair),path.c_str(),s.bx,s.bz);}
        }
        auto pct=[](std::vector<float> v,double q){std::sort(v.begin(),v.end());return v.empty()?0.f:v[size_t(q*(v.size()-1))];};
        std::printf("retail segments %zu: edge p50 %.2f p95 %.2f p99 %.2f p99.9 %.2f; tone p50 %.2f p95 %.2f p99 %.2f p99.9 %.2f\n",segs,
                    pct(edges,.5),pct(edges,.95),pct(edges,.99),pct(edges,.999),pct(tones,.5),pct(tones,.95),pct(tones,.99),pct(tones,.999));
        std::printf("strong (b) %zu (%.2f%%); pairs used by one map only %zu (%.2f%%); leave-one-out (a)+(b) candidates %zu (%.3f%%)\n",
                    strong,100.0*strong/std::max<size_t>(1,segs),unique,100.0*unique/std::max<size_t>(1,segs),candidates,100.0*candidates/std::max<size_t>(1,segs));
        return 0;
    }
    if(mode=="pairs") {
        // Score a whole section edge: piece L west/north of piece R.
        auto join=[&](const tnt::Map& l,const tnt::Map& r,int dir){
            Score sum{};int n=0;
            for(int i=0;i<16;++i) {
                const size_t a=dir==0?size_t(i)*16+15:size_t(15)*16+i,b=dir==0?size_t(i)*16:size_t(i);
                const Tile* ta=tiles.get(l.tileKeys[a],l.tileCols[a],l.tileRows[a]);
                const Tile* tb=tiles.get(r.tileKeys[b],r.tileCols[b],r.tileRows[b]);
                if(!ta||!tb)continue;
                const auto s=score(*ta,*tb,dir);sum.edge+=s.edge;sum.tone+=s.tone;++n;
            }
            if(n){sum.edge/=float(n);sum.tone/=float(n);}
            return sum;
        };
        for(int world=0;world<mapgen::kMapTypes;++world) {
            mapgen::detail::SectionCache cache(vfs);
            const auto kit=mapgen::detail::edgeKit(cache,mapgen::detail::kKitSpec[world]);
            for(int c:{3,5,10,12}) {
                const int dir=(c==3||c==12)?0:1;   // along a horizontal rim, neighbours are east/west
                for(const auto& h:kit[size_t(c)])if(h.alternate)for(const auto& h2:kit[size_t(c)])if(h2.alternate) {
                    const auto s=join(*h.map,*h2.map,dir);
                    std::printf("HILLS world=%d case=%d %s then %s edge=%.2f tone=%.2f\n",world,c,nameOf(names,h.map->tileKeys[0]).c_str(),
                                nameOf(names,h2.map->tileKeys[0]).c_str(),s.edge,s.tone);
                }
                for(const auto& h:kit[size_t(c)])if(h.alternate)for(const auto& cl:kit[size_t(c)])if(!cl.alternate) {
                    const auto before=join(*cl.map,*h.map,dir),after=join(*h.map,*cl.map,dir);
                    std::printf("PAIR world=%d case=%d hill=%s cliff=%s cliff-then-hill edge=%.2f tone=%.2f hill-then-cliff edge=%.2f tone=%.2f\n",
                        world,c,nameOf(names,h.map->tileKeys[0]).c_str(),nameOf(names,cl.map->tileKeys[0]).c_str(),
                        before.edge,before.tone,after.edge,after.tone);
                }
            }
        }
        return 0;
    }
    if(mode=="sweep"&&argc>=5) {
        const int shard=std::atoi(argv[3]),shards=std::atoi(argv[4]);
        const bool quick=argc>5&&std::string(argv[5])=="quick";
        const auto w=buildWhitelist(vfs,paths,nullptr,nullptr);
        const auto jobs=sweepJobs(quick);
        size_t maps=0,segs=0,flagged=0,candidates=0,strongOnly=0;
        const auto begin=std::chrono::steady_clock::now();
        for(size_t j=0;j<jobs.size();++j) {
            if(int(j%size_t(shards))!=shard)continue;
            const auto& job=jobs[j];
            const auto id=mapgen::encodeMapId(job.p);
            mapgen::Result r;
            try{r=mapgen::generate(job.p,vfs);}catch(const std::exception& e){std::printf("FAIL %s %s\n",id.c_str(),e.what());continue;}
            ++maps;
            for(const auto& s:segments(r.map,tiles)) {
                ++segs;
                const bool a=!w.maps.count(s.pair);
                const bool hot=s.edge>kEdge||s.tone>kTone;
                flagged+=a;candidates+=a&&hot;strongOnly+=!a&&hot;
                if(a||hot)std::printf("HIT a=%d hot=%d edge=%.2f tone=%.2f dir=%d len=%d %s | %s map=%s set=%s at=%d,%d\n",a,hot,s.edge,s.tone,
                    std::get<2>(s.pair),s.length,nameOf(names,std::get<0>(s.pair)).c_str(),nameOf(names,std::get<1>(s.pair)).c_str(),
                    id.c_str(),job.label.c_str(),s.bx,s.bz);
            }
            std::fflush(stdout);
        }
        std::printf("SUMMARY shard=%d maps=%zu segments=%zu whitelist-flagged=%zu candidates=%zu strong-b-only=%zu %.1fs\n",shard,maps,segs,
                    flagged,candidates,strongOnly,std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count());
        // The quick sweep is a regression test: no candidate seam, no strong one.
        return quick&&(candidates||strongOnly)?1:0;
    }
    std::fprintf(stderr,"unknown mode\n");return 2;
}
