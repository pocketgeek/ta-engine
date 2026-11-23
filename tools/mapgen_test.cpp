#include "tnt/mapgen.h"
#include "tnt/mapgen_internal.h"
#include "hpi/hpi.h"
#include "sim/sim.h"
#include "sim/matchsetup.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
uint64_t hash(const tak::mapgen::Result& r) {
    uint64_t h=1469598103934665603ULL;
    const auto mix=[&](uint64_t n){for(int i=0;i<8;++i){h^=(n>>(i*8))&255;h*=1099511628211ULL;}};
    const auto& m=r.map;mix(m.width);mix(m.height);mix(m.seaLevel);
    for(auto n:m.heights)mix(n);
    for(auto n:m.features)mix(n);
    for(auto n:m.tileKeys)mix(n);
    for(auto n:m.tileCols)mix(n);
    for(auto n:m.tileRows)mix(n);
    for(const auto& s:m.featureNames){for(unsigned char c:s)mix(c);mix(0);}
    for(auto [x,z]:r.starts){mix(x);mix(z);}
    return h;
}
int manaCount(const tak::mapgen::Result& r) {
    int count=0;
    for(auto f:r.map.features)
        if(f<r.map.featureNames.size() && tak::hpi::MountSet::key(r.map.featureNames[f]).find("mana")!=std::string::npos)++count;
    return count;
}
// Industrial Zone stones on a pad (lava within 20 cells on three or four sides): the
// stone's middle sits midway (within a cell) between the lava on each side it closes in, so
// it is in the pad's middle, not off to one side. Returns the stones checked.
int padStonesCentred(const tak::mapgen::Result& r) {
    const auto& m=r.map;int checked=0;
    for(int z=0;z<m.height;++z)for(int x=0;x<m.width;++x) {
        const auto f=m.features[size_t(z)*m.width+x];
        if(f>=m.featureNames.size()||tak::hpi::MountSet::key(m.featureNames[f]).find("mana")==std::string::npos)continue;
        // Cells from the stone's 2x2 middle to the first lava cell along a ray.
        const auto reach=[&](int dx,int dz) {
            for(int n=1;n<=20;++n) {
                const int cx=dx<0?x-n:dx>0?x+1+n:x,cz=dz<0?z-n:dz>0?z+1+n:z;
                if(cx<0||cz<0||cx>=m.width||cz>=m.height)return 99;
                if(m.features[size_t(cz)*m.width+cx]==0xfffc)return n;
            }
            return 99;
        };
        const int west=reach(-1,0),east=reach(1,0),north=reach(0,-1),south=reach(0,1);
        if((west<99)+(east<99)+(north<99)+(south<99)<3)continue;
        ++checked;
        if(west<99&&east<99)check(std::abs(west-east)<=1,"pad stone off its pad's middle (west-east)");
        if(north<99&&south<99)check(std::abs(north-south)<=1,"pad stone off its pad's middle (north-south)");
    }
    return checked;
}
void manaSlider(const tak::hpi::Vfs& vfs) {
    using namespace tak::mapgen;
    for(const auto [size,players]:std::array<std::pair<int,int>,3>{{{256,2},{512,2},{2048,8}}}) {
        int previous=-1;
        for(int density:{0,43,85,128,170,213,255}) {
            Params p;p.widthCells=p.heightCells=uint16_t(size);p.players=uint8_t(players);
            p.waterDensity=p.reliefDensity=p.treeDensity=p.rockDensity=0;p.manaDensity=uint8_t(density);
            const auto r=generate(p,vfs);const int count=manaCount(r),rounds=extraManaRounds(p.manaDensity);
            check(count>=previous,"raising extra mana removed deposits");
            check(count>=players*3 && count<=players*(3+rounds) && count%players==0,"extra mana count violates balanced-round limit");
            if(!density)check(count==players*3,"zero extra mana changed guaranteed home deposits");
            if(size>=512)check(count==players*(3+rounds),"roomy flat map did not honor extra mana setting");
            std::printf("mana slider size=%d players=%d requested=%d actual=%d\n",size,players,rounds,count);
            previous=count;
        }
    }
}
// v9 scattered neutral mana. None must leave the map exactly as it was; each level
// must scale with map AREA (not player count), roughly double the level below, and
// Max must approach a six-spot home's density (6 / 87^2).
void scatteredMana(const tak::hpi::Vfs& vfs) {
    using namespace tak::mapgen;
    Params base;base.players=8;base.seed=0xfedcba9876543210ULL;
    base.waterDensity=base.reliefDensity=0;base.treeDensity=base.rockDensity=base.manaDensity=255;
    base.plateauDensity=base.mountainDensity=base.townDensity=0;
    const auto none=generate(base,vfs);
    check(hash(none)==0x6bf977a42891c638ULL && none.scatteredMana==0,"scattered mana None changed the map");
    Params named;named.name=std::string(24,'n');named.manaScatter=ScatterHigh;
    const auto namedId=encodeMapId(named);
    check(decodeMapId(namedId).manaScatter==ScatterHigh && decodeMapId(namedId).name==named.name,"v9 codec mismatch");
    named.manaScatter=200;check(decodeMapId(encodeMapId(named)).manaScatter==ScatterMax,"scatter level unclamped");
    const double homeDensity=6.0/(87*87);
    for(const int players:{2}) {
        int previous[2]={0,0};
        for(int level=ScatterNone;level<kScatterLevels;++level) {
            int counts[2];
            for(int k=0;k<2;++k) {
                Params p;p.players=uint8_t(players);p.widthCells=p.heightCells=uint16_t(512<<k);p.seed=7;
                p.waterDensity=p.reliefDensity=p.manaDensity=0;p.treeDensity=p.rockDensity=128;p.manaScatter=uint8_t(level);
                const auto r=generate(p,vfs);counts[k]=r.scatteredMana;
                check(manaCount(r)==players*3+r.scatteredMana,"scattered count disagrees with the map");
                check(hash(r)==hash(generate(decodeMapId(encodeMapId(p)),vfs)),"scattered mana not deterministic");
                const double density=double(r.scatteredMana)/(double(p.widthCells)*p.heightCells);
                std::printf("scattered mana players=%d size=%d level=%s count=%d density/home=%.2f\n",
                            players,int(p.widthCells),scatterName(uint8_t(level)),r.scatteredMana,density/homeDensity);
                if(level==ScatterMax && k==1) check(density>0.6*homeDensity && density<1.1*homeDensity,"Max is not a home's mana density");
            }
            if(!level) {check(counts[0]==0&&counts[1]==0,"None scattered mana");continue;}
            // Four times the area: about four times the spots (homes take a share of the small map).
            check(counts[1]>=3*counts[0] && counts[1]<=6*counts[0],"scattered mana does not follow map area");
            if(level>ScatterMinimum) for(int k=0;k<2;++k)
                check(counts[k]*10>=previous[k]*15 && counts[k]*10<=previous[k]*26,"levels do not roughly double");
            previous[0]=counts[0];previous[1]=counts[1];
        }
    }
}
void syntheticMaps() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("tak-mapgen-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);} } cleanup{root};
    fs::create_directories(root/"features"/"aramon");
    {
        std::ofstream out(root/"features"/"aramon"/"fixture.tdf");
        for(const auto& [kind,count]:std::array<std::pair<std::string,int>,4>{{{"Tree",10},{"Rock",7},{"Henge",9},{"Mana",3}}})
            for(int i=1;i<=count;++i) {
                const std::string name="Ara"+kind+(i<10?"0":"")+std::to_string(i);
                const bool mana=kind=="Mana";
                out<<"["<<name<<"] {\nfootprintx="<<(mana?2:3)<<";\nfootprintz="<<(mana?2:4)
                   <<";\nblocking="<<(!mana)<<"; }\n";
            }
    }
    tak::hpi::Vfs vfs;vfs.addLayer(tak::hpi::MountSet(root));
    for(int density=0;density<=255;++density) {
        const int rounds=tak::mapgen::extraManaRounds(uint8_t(density));
        check(rounds>=0 && rounds<=6,"extra mana scale is unbounded");
        if(density)check(rounds>=tak::mapgen::extraManaRounds(uint8_t(density-1)),"extra mana scale is not monotone");
    }
    check(tak::mapgen::extraManaRounds(0)==0 && tak::mapgen::extraManaRounds(128)==3 &&
          tak::mapgen::extraManaRounds(255)==6,"extra mana endpoints/default changed");
    for(int players=2;players<=8;++players) {
        tak::mapgen::Params p;p.players=players;p.seed=0xfedcba9876543210ULL;
        p.waterDensity=p.reliefDensity=0;p.treeDensity=p.rockDensity=p.manaDensity=255;
        const auto r=tak::mapgen::generate(p,vfs);
        const auto repeated=tak::mapgen::generate(tak::mapgen::decodeMapId(tak::mapgen::encodeMapId(p)),vfs);
        check(hash(r)==hash(repeated),"synthetic map not deterministic");
        check(r.waterPercent==0&&r.reliefPatches==0,"dry flat synthetic map changed terrain");
        // The synthetic install has no kit sections: the default kit amounts are a no-op.
        check(r.plateaus==0&&r.mountains==0&&r.towns==0&&r.kitFallback==0,"kits appeared without their sections");
        auto bare=p;bare.plateauDensity=bare.mountainDensity=bare.townDensity=0;
        check(hash(tak::mapgen::generate(bare,vfs))==hash(r),"kit amounts changed a map without kit sections");
        if(players==8) {
            check(hash(r)==0x6bf977a42891c638ULL,"cross-platform generator golden changed");
            std::printf("synthetic generator golden: %016llx\n",(unsigned long long)hash(r));
        }
        // Non-square feature footprints are deliberately unlike the old guessed
        // sizes. No two procedural anchors may claim the same cell.
        std::vector<uint8_t> occupied(r.map.heights.size());
        for(int z=0;z<r.map.height;++z)for(int x=0;x<r.map.width;++x) {
            const auto f=r.map.features[size_t(z)*r.map.width+x];if(f>=r.map.featureNames.size())continue;
            const bool mana=r.map.featureNames[f].find("Mana")!=std::string::npos;
            for(int dz=0;dz<(mana?2:4);++dz)for(int dx=0;dx<(mana?2:3);++dx) {
                check(x+dx<r.map.width&&z+dz<r.map.height,"feature extends off map");
                auto& cell=occupied[size_t(z+dz)*r.map.width+x+dx];
                if(cell) {
                    std::fprintf(stderr,"overlap players=%d feature=%s origin=%d,%d at=%d,%d previous=%s\n",players,
                        r.map.featureNames[f].c_str(),x,z,x+dx,z+dz,r.map.featureNames[cell-1].c_str());
                    throw std::runtime_error("generated feature footprints overlap");
                }
                cell=uint8_t(f+1);
            }
        }
    }
    manaSlider(vfs);
    scatteredMana(vfs);
}
void verify(const tak::mapgen::Params& input,const tak::mapgen::Result& r) {
    const auto p=tak::mapgen::sanitize(input);const auto& m=r.map;
    check(m.width==p.widthCells&&m.height==p.heightCells,"wrong dimensions");
    check(r.starts.size()==p.players,"wrong start count");
    check(m.features.size()==m.heights.size()&&m.heights.size()==size_t(m.width)*m.height,"wrong cell planes");
    check(m.tileKeys.size()==size_t(m.width/2)*(m.height/2),"wrong tile plane");
    check(p.waterDensity||tak::mapgen::automaticWater(p.layout)||r.waterPercent==0,"zero water produced water");
    check(p.layout!=tak::mapgen::Lakes||!p.waterDensity||r.waterPercent>0,"lake layout has no water");
    check(p.reliefDensity||r.reliefPatches==0,"zero relief produced hills");
    check(tak::mapgen::worldHasHills(p.mapType)||r.reliefPatches==0,"a world without hill sections got hills");
    if(!tak::mapgen::worldHasForests(p.mapType))
        for(const auto& name:r.map.featureNames)
            check(tak::hpi::MountSet::key(name).find("tree")==std::string::npos,"a world without trees got a forest");
    check(p.plateauDensity||p.mountainDensity||tak::mapgen::automaticPlateaus(p.layout)||r.plateaus==0,"zero plateaus produced plateaus");
    check(p.mountainDensity||r.mountains==0,"zero mountains produced mountains");
    check(p.townDensity||r.towns==0,"zero towns produced towns");
    // Lava is never open ground: retail blocks every cell of it (0xfffc).
    if(tak::mapgen::lavaWorld(p.mapType))
        for(size_t i=0;i<m.heights.size();++i)
            check(m.heights[i]>=m.seaLevel||m.features[i]==0xfffc,"open lava");
    for(size_t i=0;i<r.starts.size();++i) {
        const auto [x,z]=r.starts[i];
        for(size_t j=0;j<i;++j) {
            const int dx=x-r.starts[j].first,dz=z-r.starts[j].second;
            check(dx*dx+dz*dz>=72*72,"starts too close");
        }
        const auto level=m.heights[size_t(z)*m.width+x];
        check(level>m.seaLevel,"underwater start");
        for(int dz=-12;dz<12;++dz)for(int dx=-12;dx<12;++dx) {
            const size_t c=size_t(z+dz)*m.width+x+dx;
            check(m.heights[c]==level,"base isn't flat");
            check(m.features[c]==0xffff,"base has feature anchors");
        }
        const std::array<std::pair<int,int>,3> offsets={{{-24,-24},{24,-24},{0,28}}};
        for(int tier=0;tier<3;++tier) {
            const auto [dx,dz]=offsets[tier];const auto f=m.features[size_t(z+dz)*m.width+x+dx];
            check(f<m.featureNames.size(),"missing home mana");
            const auto name=tak::hpi::MountSet::key(m.featureNames[f]);
            check(name.ends_with("mana0"+std::to_string(tier+1)),"wrong home mana tier");
        }
    }
    int mana=0;
    for(auto f:m.features) {
        if(f>=0xfffb)continue;
        check(f<m.featureNames.size(),"invalid feature index");
        if(tak::hpi::MountSet::key(m.featureNames[f]).find("mana")!=std::string::npos)++mana;
    }
    for (int z=0;z<m.height;++z) for (int x=0;x<m.width;++x) {
        const auto f=m.features[size_t(z)*m.width+x];
        if (f>=m.featureNames.size() || tak::hpi::MountSet::key(m.featureNames[f]).find("mana")==std::string::npos) continue;
        bool ruins=false;
        for (int dz=-24;dz<=24 && !ruins;++dz) for (int dx=-24;dx<=24;++dx) {
            const int nx=x+dx,nz=z+dz;
            if(nx<0||nz<0||nx>=m.width||nz>=m.height)continue;
            const auto r=m.features[size_t(nz)*m.width+nx];
            if(r<m.featureNames.size() && tak::hpi::MountSet::key(m.featureNames[r]).find("henge")!=std::string::npos) { ruins=true; break; }
        }
        check(ruins||tak::mapgen::lavaWorld(p.mapType),"mana spot has no surrounding ruins");   // volcano deposits are bare
    }
    mana-=r.scatteredMana+r.padMana;
    check(mana>=p.players*3&&mana%p.players==0,"unequal resource rounds");
    check(p.layout==tak::mapgen::IndustrialZone||r.padMana==0,"pad mana off Industrial Zone");
    check((p.layout!=tak::mapgen::Islands&&p.layout!=tak::mapgen::Ports)||r.harbors.size()==p.players,"missing island harbors");
}
}
int main(int argc,char** argv) {
    try {
        using namespace tak::mapgen;
        for(int layout=0;layout<3;++layout)for(int players=2;players<=8;++players) {
            Params p;p.layout=layout;p.players=players;p.seed=0xfedcba9876543210ULL;
            p.widthCells=768;p.heightCells=640;
            p.plateauDensity=uint8_t(players*30);p.mountainDensity=uint8_t(255-players);p.townDensity=uint8_t(layout*100);
            const auto id=encodeMapId(p);const auto d=decodeMapId(id);const auto expected=sanitize(p);
            check(d.formatVer==10&&d.seed==p.seed&&d.players==p.players&&d.widthCells==expected.widthCells&&d.heightCells==expected.heightCells,"seed codec mismatch");
            check(d.treeDensity==p.treeDensity&&d.rockDensity==p.rockDensity&&d.layout==p.layout,"density/layout codec mismatch");
            check(d.plateauDensity==p.plateauDensity&&d.mountainDensity==p.mountainDensity&&d.townDensity==p.townDensity,"kit codec mismatch");
        }
        // Only the current recipe version with its exact payload is well formed. An id
        // of another version (v7-v9 shapes, a v11 number) or length, or with a non-hex
        // digit, decodes to the defaults, and parseMapId refuses it instead of building
        // a different map from the same seed.
        const std::string current=encodeMapId(Params{});
        check(encodeMapId(parseMapId(current))==current,"the current recipe was refused");
        std::string otherVersion=current;otherVersion.replace(6,4,"0b00");
        std::string badDigit=current;badDigit.back()='g';
        for(const std::string& other:{std::string("~gen1~080001000000000000000100030003020000ff608003"),
                                      std::string("~gen1~09000100000000000000038001800108ffffffffff0004"),
                                      std::string("~gen1~0700010000000000000001000300030200"),
                                      otherVersion,current.substr(0,current.size()-2),current+std::string(50,'4'),
                                      current+"4",badDigit,std::string("~gen1~")}) {
            check(encodeMapId(decodeMapId(other))==encodeMapId(Params{}),"a malformed recipe did not decode to the defaults");
            bool refused=false;
            try {parseMapId(other);}
            catch(const std::runtime_error& e) {refused=std::string(e.what()).find("generate a new map")!=std::string::npos;}
            check(refused,"a malformed recipe was accepted");
        }
        // A high-bit seed and invalid input must not wrap dimensions or allow a new
        // layout to reinterpret a seed.
        for(int layout=Maze;layout<kLayouts;++layout) {
            Params p;p.layout=layout;p.name="Themed";
            const auto d=decodeMapId(encodeMapId(p));
            check(d.layout==layout && (themedLayout(d.mapType)==layout || (layout==IndustrialZone && d.mapType==Volcano)),
                  "theme codec/world mismatch");
        }
        Params invalid;invalid.widthCells=0;invalid.heightCells=65535;invalid.players=255;invalid.layout=255;
        auto sane=sanitize(invalid);check(sane.players==8&&sane.widthCells>=256&&sane.heightCells==2048&&sanitize(sane).widthCells==sane.widthCells,"sanitize bounds");
        Params named; named.name = "  My Lake-7_  ";
        check(decodeMapId(encodeMapId(named)).name == "My Lake-7_", "map name round trip");
        check(friendlyLabel(named) == "My Lake-7_", "map name display");
        named.name = "../bad;[name]\\test";
        check(sanitize(named).name == "badnametest", "unsafe map name");
        named.name = std::string(100, 'a');
        check(decodeMapId(encodeMapId(named)).name.size() == 24, "map name length");
        named.name = std::string(24, 'z');named.plateauDensity = 255;
        check(decodeMapId(encodeMapId(named)).name == named.name && decodeMapId(encodeMapId(named)).plateauDensity == 255,
              "longest name with kits round trip");
        syntheticMaps();
        if(argc<2){std::puts("mapgen codec/bounds passed (retail sweep takes a data path)");return 0;}
        const auto vfs=tak::hpi::mountRetailRoot(argv[1]);
        if(argc>2 && std::string(argv[2])=="--mana") {
            Params cramped;cramped.mapType=Veruna;cramped.players=7;cramped.widthCells=cramped.heightCells=384;
            const auto limited=generate(cramped,vfs);verify(cramped,limited);
            check(manaCount(limited)%7==0,"omitted expansion left a partial resource round");
            check(hash(limited)==hash(generate(cramped,vfs)),"omitted expansion is not deterministic");
            for(int world=0;world<kMapTypes;++world)for(int layout:{int(Mainland),int(themedLayout(uint8_t(world)))}) {
                int previous=-1;
                for(int density:{0,128,255}) {
                    Params p;p.mapType=uint8_t(world);p.layout=uint8_t(layout);p.widthCells=p.heightCells=1024;
                    p.players=2;p.manaDensity=uint8_t(density);
                    const auto r=generate(p,vfs);verify(p,r);const int count=manaCount(r);
                    check(count>=previous,"authored terrain reduced mana when slider increased");
                    check(count>=6 && count<=2*(3+extraManaRounds(p.manaDensity)),"authored terrain ignored extra mana limit");
                    std::printf("mana world=%d layout=%d density=%d count=%d\n",world,layout,density,count);
                    previous=count;
                }
                check(previous>6,"roomy authored map never added extra mana");
            }
            std::puts("extra mana slider passed");return 0;
        }
        if(argc>2 && std::string(argv[2])=="--kits") {
            // v10 retail kits on every world and layout: deterministic, fair (the
            // generator's own checks plus an independent flood below), and every
            // stamped kit section carries its retail heights with its artwork.
            std::map<uint32_t,const tak::tnt::Map*> pieceByKey;
            tak::mapgen::detail::SectionCache sections(vfs);
            for(const auto& dir:tak::mapgen::detail::kitDirectories())
                for(const auto& path:tak::hpi::MountSet::key(dir).ends_with(".tnt")?std::vector<std::string>{dir}:sections.list(dir))
                    if(const auto* piece=sections.load(path);piece&&piece->width==32&&piece->height==32)
                        if(std::all_of(piece->tileKeys.begin(),piece->tileKeys.end(),[&](uint32_t k){return k==piece->tileKeys[0];}))
                            pieceByKey.emplace(piece->tileKeys[0],piece);
            int seen[kMapTypes][kKits]={};
            for(int world=0;world<kMapTypes;++world)
            for(int layout:{int(Mainland),int(Lakes),int(Islands),int(themedLayout(uint8_t(world)))})
            for(int players:{2,5,8})for(int seed=1;seed<=2;++seed)for(int amount:{-1,255}) {
                Params p;p.mapType=uint8_t(world);p.layout=uint8_t(layout);p.players=uint8_t(players);p.seed=uint64_t(seed);
                p.widthCells=p.heightCells=1024;
                if(amount>=0)p.plateauDensity=p.mountainDensity=p.townDensity=uint8_t(amount);
                p=sanitize(p);
                const auto begin=std::chrono::steady_clock::now();
                const auto r=generate(p,vfs);verify(p,r);
                const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
                check(hash(r)==hash(generate(decodeMapId(encodeMapId(p)),vfs)),"kit recipe not deterministic");
                check(r.kitFallback==0,"kits had to be reduced to fit a fair map");
                seen[world][KitPlateaus]+=r.plateaus;seen[world][KitMountains]+=r.mountains;seen[world][KitTowns]+=r.towns;
                const auto& m=r.map;
                // Independent flood with the generator's validation class: every start
                // reaches every other (not across Islands' sea) and all of its mana.
                tak::sim::NavGrid::Limits limits;limits.maxSlope=16;limits.maxWaterDepth=0;
                tak::sim::NavGrid ground(m.heights,m.width,m.height,m.seaLevel,limits);
                std::vector<uint8_t> blocked(m.heights.size());
                for(size_t i=0;i<blocked.size();++i)blocked[i]=m.features[i]==0xfffc;
                for(int z=0;z<m.height;++z)for(int x=0;x<m.width;++x) {
                    const auto f=m.features[size_t(z)*m.width+x];if(f>=m.featureNames.size())continue;
                    const auto name=tak::hpi::MountSet::key(m.featureNames[f]);
                    if(name.find("mana")!=std::string::npos)continue;
                    // Blocking scenery: a conservative 3x3 claim (real footprints are smaller or equal for trees/rocks).
                    if(name.find("tree")!=std::string::npos||name.find("rock")!=std::string::npos||name.find("henge")!=std::string::npos)
                        for(int dz=0;dz<2;++dz)for(int dx=0;dx<2;++dx)if(x+dx<m.width&&z+dz<m.height)blocked[size_t(z+dz)*m.width+x+dx]=1;
                }
                ground.setObstacles(&blocked);
                std::vector<int> d(m.heights.size(),-1),q;
                const auto [sx,sz]=r.starts[0];d[size_t(sz)*m.width+sx]=0;q.push_back(sz*m.width+sx);
                for(size_t head=0;head<q.size();++head) {
                    const int x=q[head]%m.width,z=q[head]/m.width;
                    for(auto [dx,dz]:std::array<std::pair<int,int>,4>{{{1,0},{-1,0},{0,1},{0,-1}}}) {
                        const int nx=x+dx,nz=z+dz;
                        if(nx<0||nz<0||nx>=m.width||nz>=m.height||d[size_t(nz)*m.width+nx]>=0||!ground.fits(nx,nz,2))continue;
                        d[size_t(nz)*m.width+nx]=d[size_t(z)*m.width+x]+1;q.push_back(nz*m.width+nx);
                    }
                }
                if(p.layout!=Islands)for(auto [x,z]:r.starts)check(d[size_t(z)*m.width+x]>=0,"cliffs cut a start off");
                // Art and heights together: a section whose tiles are one whole kit
                // piece must carry that piece's heights exactly.
                int sectionsChecked=0;
                for(int sz2=0;sz2<m.height/32;++sz2)for(int sx2=0;sx2<m.width/32;++sx2) {
                    const size_t first=size_t(sz2*16)*m.blocksX+sx2*16;
                    const auto it=pieceByKey.find(m.tileKeys[first]);if(it==pieceByKey.end())continue;
                    bool whole=true;
                    for(int bz=0;whole&&bz<16;++bz)for(int bx=0;bx<16;++bx) {
                        const size_t b=size_t(sz2*16+bz)*m.blocksX+sx2*16+bx,pb=size_t(bz)*16+bx;
                        if(m.tileKeys[b]!=it->second->tileKeys[pb]||m.tileCols[b]!=it->second->tileCols[pb]||m.tileRows[b]!=it->second->tileRows[pb]){whole=false;break;}
                    }
                    if(!whole)continue;
                    ++sectionsChecked;
                    for(int z=0;z<32;++z)for(int x=0;x<32;++x)
                        check(m.heights[size_t(sz2*32+z)*m.width+sx2*32+x]==it->second->heights[size_t(z)*32+x],"kit section artwork without its heights");
                }
                std::printf("kits world=%d layout=%s players=%d seed=%d amount=%d plateaus=%d mountains=%d towns=%d sections=%d %.2fs\n",
                            world,layoutName(uint8_t(layout)),players,seed,amount,r.plateaus,r.mountains,r.towns,sectionsChecked,seconds);
                std::fflush(stdout);
            }
            // The sliders act independently: Mountains 0 still raises plain plateaus,
            // Plateaus 0 leaves only the mountains' own plateaus, and high ground the
            // kits build carries mana when a fair round fits (two players, roomy maps).
            int plateauMana=0;
            for(int world=0;world<kMapTypes;++world)for(int seed=1;seed<=4;++seed) {
                Params p;p.mapType=uint8_t(world);p.widthCells=p.heightCells=768;p.seed=uint64_t(seed);
                p.mountainDensity=0;
                const auto plain=generate(p,vfs);verify(p,plain);
                check(plain.mountainPlateaus==0&&plain.mountains==0&&(plain.plateaus>0||!kitAvailable(uint8_t(world),KitPlateaus)),
                      "Mountains 0 lost the plain plateaus");
                plateauMana+=plain.plateauMana;
                p.plateauDensity=0;p.mountainDensity=255;
                const auto ranges=generate(p,vfs);verify(p,ranges);
                check(ranges.plateaus==ranges.mountainPlateaus,"Plateaus 0 still raised a plain plateau");
                check((ranges.mountains>0)==kitAvailable(uint8_t(world),KitMountains)||seed>1,"Mountains 255 raised no range");
                std::printf("kits sliders world=%d seed=%d plain: plateaus=%d topmana=%d; ranges only: plateaus=%d mountains=%d\n",
                            world,seed,plain.plateaus,plain.plateauMana,ranges.plateaus,ranges.mountains);
            }
            check(plateauMana>0,"no plateau ever carried mana");
            for(int world=0;world<kMapTypes;++world) {
                check((seen[world][KitPlateaus]>0)==kitAvailable(uint8_t(world),KitPlateaus),"plateaus disagree with the kit survey");
                check((seen[world][KitTowns]>0)==kitAvailable(uint8_t(world),KitTowns),"towns disagree with the kit survey");
                check((seen[world][KitMountains]>0)==kitAvailable(uint8_t(world),KitMountains),"mountains disagree with the kit survey");
            }
            // A map with no room for a whole range still gets a compact peak: 384 cells
            // square, two players, raised mountains on one seed in six before.
            {
                int peaks=0;
                for(int seed=1;seed<=6;++seed) {
                    Params p;p.widthCells=p.heightCells=384;p.seed=uint64_t(seed*7919);
                    const auto r=generate(p,vfs);verify(p,r);peaks+=r.mountains>0;
                }
                std::printf("compact peaks on 384-cell two-player maps: %d of 6\n",peaks);
                check(peaks>=4,"a small map found no room for even a compact peak");
            }
            // All three at zero is the v9 terrain: no kit sections at all.
            Params zero;zero.plateauDensity=zero.mountainDensity=zero.townDensity=0;zero.widthCells=zero.heightCells=768;
            const auto plain=generate(zero,vfs);
            check(plain.plateaus==0&&plain.mountains==0&&plain.towns==0,"zero kit amounts still placed kits");
            std::puts("retail terrain kits passed");return 0;
        }
        if(argc>2&&(std::string(argv[2])=="--naval"||std::string(argv[2])=="--ports")) {
            for(bool crusades:{false,true}) {
                tak::sim::TypeRegistry registry;tak::sim::setupRegistry(registry,vfs,crusades);
                const auto* fort=registry.find("verasy");check(fort,"Sea Fort definition");
                for(int world=0;world<5;++world) {
                    const bool ports=std::string(argv[2])=="--ports";
                    if(ports && world!=Veruna)continue;
                    Params p;p.mapType=world;p.layout=ports?Ports:Islands;p.players=2;
                    const auto generated=generate(p,vfs);
                    for(const auto& name:registry.buildable("verasy")) {
                        const auto* ship=registry.find(name);check(ship,"ship definition");
                        tak::sim::World simulation;
                        tak::sim::MatchConfig cfg;cfg.vfs=&vfs;cfg.mapPath=encodeMapId(p);
                        cfg.slots={{true,2,0,1.f,false,false}};
                        tak::sim::setupMatch(simulation,registry,cfg);
                        for(const auto [x,z]:generated.harbors)
                            check(simulation.canPlace(fort,float(x*16),float(z*16)),"harbor cannot fit actual Sea Fort yard");
                        const auto [x,z]=generated.harbors[0];
                        const int id=simulation.spawn(fort,float(x*16),float(z*16),std::nullopt,0);
                        simulation.train(id,ship,2);
                        for(int tick=0;tick<18000&&!simulation.unit(id)->buildQueue.empty();++tick) {
                            simulation.player(0).mana=100000;simulation.tick(1.f/30);
                        }
                        int complete=0;
                        for(const auto& u:simulation.units())complete+=u.alive()&&u.type==ship&&!u.underConstruction;
                        std::printf("naval world=%d balance=%d ship=%s complete=%d\n",world,crusades,name.c_str(),complete);
                        check(complete==2&&simulation.unit(id)->buildQueue.empty(),"generated harbor ship production failed");
                    }
                }
            }
            std::puts("generated-map naval production passed");return 0;
        }
        if (argc>2 && std::string(argv[2])=="--large") {
            for(int world=0;world<5;++world) for(int layout=0;layout<3;++layout) {
                Params p;p.widthCells=p.heightCells=2048;p.players=8;p.mapType=world;p.layout=layout;
                const auto begin=std::chrono::steady_clock::now();
                const auto r=generate(p,vfs); verify(p,r);
                check(hash(r)==hash(generate(decodeMapId(encodeMapId(p)),vfs)),"large map recipe changed");
                std::printf("large map world=%d layout=%d hash=%016llx %.2fs\n",world,layout,
                    (unsigned long long)hash(r),std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count());
                std::fflush(stdout);
            }
            return 0;
        }
        // Never place grass/cobbles relief directly into another ground family.
        for(int world:{int(Taros),int(Veruna)}) {
            std::vector<uint32_t> incompatible;
            for(const auto& path:vfs.list(world==Taros?"sections/taros/low specials":"sections/veruna/low specials")) {
                const auto key=tak::hpi::MountSet::key(path);
                if(!(key.ends_with(".tnt") && ((world==Veruna && key.find("/breaker")!=std::string::npos) ||
                    key.ends_with("/unique23.tnt")||key.ends_with("/unique25.tnt")||key.ends_with("/unique27.tnt"))))continue;
                auto piece=tak::tnt::Map::load(vfs.read(path));
                incompatible.push_back(piece.tileKeys[piece.tileKeys.size()/2]);
            }
            Params p;p.mapType=world;p.widthCells=p.heightCells=768;p.reliefDensity=255;p.waterDensity=0;
            const auto r=generate(p,vfs);verify(p,r);
            for(auto key:r.map.tileKeys) check(std::find(incompatible.begin(),incompatible.end(),key)==incompatible.end(),"incompatible relief surface used");
        }
        // Lakes may meet the edge; there is no guaranteed dry rim.
        int edgeWet=0;
        for(int seed=1;seed<=5;++seed) {
            Params p;p.layout=Lakes;p.waterDensity=255;p.seed=seed;
            const auto r=generate(p,vfs);verify(p,r);const auto& m=r.map;
            for(int x=0;x<m.width;++x)edgeWet+=m.heights[x]<m.seaLevel || m.heights[size_t(m.height-1)*m.width+x]<m.seaLevel;
            for(int z=0;z<m.height;++z)edgeWet+=m.heights[size_t(z)*m.width]<m.seaLevel || m.heights[size_t(z)*m.width+m.width-1]<m.seaLevel;
        }
        check(edgeWet>0,"lakes still have a guaranteed dry rim");
        if(argc>2&&std::string(argv[2])=="--themes") {
            int deadEndMana=0,padMana=0,padStones=0;
            for(int layout=Maze;layout<kLayouts;++layout)for(int players:{2,3,4,5,6,7,8})for(int seed=1;seed<=3;++seed) {
                Params p;p.layout=layout;p.players=players;p.seed=seed;
                if(seed==2)p.treeDensity=p.rockDensity=p.manaDensity=p.reliefDensity=0;
                if(seed==3)p.treeDensity=p.rockDensity=p.manaDensity=p.reliefDensity=255;
                p=sanitize(p);
                const auto r=generate(p,vfs);verify(p,r);
                check(hash(r)==hash(generate(decodeMapId(encodeMapId(p)),vfs)),"themed recipe not deterministic");
                if(layout==Maze||(layout==Highlands&&p.reliefDensity)) check(*std::max_element(r.map.heights.begin(),r.map.heights.end())>200,"themed uplands absent");
                if(layout==Maze) {
                    const auto& m=r.map;
                    const auto low=[&](int x,int z){return m.heights[size_t(z)*m.width+x]<200;};
                    for(int x=96;x<=m.width-96;x+=128)
                        check(low(x,0)&&low(x,m.height-1),"maze north/south exits blocked");
                    for(int z=96;z<=m.height-96;z+=128)
                        check(low(0,z)&&low(m.width-1,z),"maze west/east exits blocked");
                }
                if(layout==Ports||layout==Riverlands||layout==LavaFlows||layout==IndustrialZone) check(r.waterPercent>0,"themed waterways absent");
                // An Industrial Zone's neutral mana is its dead-end pads (no scatter asked for).
                if(layout==IndustrialZone) {
                    deadEndMana+=r.scatteredMana;padMana+=r.padMana;
                    padStones+=padStonesCentred(r);
                    // LAVA PADS at NONE: no pads, so no flyers' mana.
                    Params none=p;none.waterDensity=0;
                    const auto bare=generate(none,vfs);verify(none,bare);
                    check(bare.padMana==0,"lava pads at NONE still hold mana");
                }
                else if(!p.manaScatter) check(r.scatteredMana==0,"mana scattered unasked");
                std::printf("theme=%s players=%d seed=%d water=%d harbors=%zu passed\n",layoutName(layout),players,seed,r.waterPercent,r.harbors.size());std::fflush(stdout);
            }
            std::printf("industrial zone dead-end mana: %d\n",deadEndMana);
            std::printf("industrial zone pad mana: %d, centred pad stones: %d\n",padMana,padStones);
            check(padStones>=padMana,"industrial zone pad stones not found");
            check(deadEndMana>=10,"industrial zone dead ends carry too little mana");
            check(padMana>=10,"industrial zone lava pads carry too little mana");
            return 0;
        }
        const bool sweep=argc>2&&std::string(argv[2])=="--sweep";
        const int seeds=sweep?5:1;int count=0,failures=0,patches=0;
        const auto begin=std::chrono::steady_clock::now();
        for(int world=0;world<kMapTypes;++world)for(int layout=0;layout<3;++layout)
        for(int players=2;players<=8;++players)for(int seed=1;seed<=seeds;++seed)
        for(int size:(sweep?std::vector<int>{256,512,768}:std::vector<int>{384}))
        for(int density:(sweep?std::vector<int>{0,128,255}:std::vector<int>{128})) {
            Params p;p.mapType=world;p.layout=layout;p.players=players;p.seed=seed;
            p.widthCells=p.heightCells=size;
            p.treeDensity=p.rockDensity=p.manaDensity=p.waterDensity=p.reliefDensity=density;
            p.plateauDensity=p.mountainDensity=p.townDensity=uint8_t(density);
            try {
                const auto r=generate(p,vfs);verify(p,r);patches+=r.reliefPatches;
                if(argc>2&&std::string(argv[2])=="--hashes")
                    std::printf("map hash world=%d layout=%d players=%d seed=%d: %016llx\n",
                        world,layout,players,seed,(unsigned long long)hash(r));
                if(!sweep||count%97==0)check(hash(r)==hash(generate(decodeMapId(encodeMapId(p)),vfs)),"non-repeatable map");
                if(argc>3&&seed==1&&players==3&&size==512&&density==128) {
                    const auto bytes=r.map.save();
                    const auto file=std::string(argv[3])+"/world"+std::to_string(world)+"-layout"+std::to_string(layout)+".tnt";
                    std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                }
                ++count;
            } catch(const std::exception& e) {
                std::printf("FAIL world=%d layout=%d players=%d seed=%d size=%d density=%d: %s\n",world,layout,players,seed,size,density,e.what());
                ++failures;
            }
            std::fflush(stdout);
        }
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        std::printf("mapgen: %d passed, %d failed, %d authored relief patches; %.3fs\n",count,failures,patches,seconds);
        return failures?1:0;
    } catch(const std::exception& e){std::fprintf(stderr,"mapgen: %s\n",e.what());return 1;}
}
