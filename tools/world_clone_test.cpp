// World::clone / World::restoreFrom: a copy must be exact and separate. Exact means it
// runs on as the original would, hash for hash, on every tick; separate means neither
// can reach the other afterwards. Replay seeking stands on both (client/replaybar.h).
#include "navigation_test_common.h"
#include "client/alloccount.h"
#include "hpi/hpi.h"
#include "sim/legion.h"
#include "sim/matchsetup.h"
#include "sim/mission.h"
#include <cstdlib>
#include <memory>
#include <random>
#include <string>

using namespace navigation_test;

namespace {
// copyIsSmall's ceilings: a copy of its fixture took 77.5 bytes a map cell when it carried everything. One that
// runs takes 48.5 now (what is left is state: the occupancy, ownership and sight maps, Legion's soft stamps,
// routes), and a keyframe, which holds those maps as runs, 6.6.
constexpr double kCopyBytesPerCell=60,kKeyframeBytesPerCell=12;
void lockstep(World& a,World& b,int ticks,const char* what) {
    for(int t=0;t<ticks;++t) {
        a.tick(1.f/30);b.tick(1.f/30);
        if(a.stateHash()!=b.stateHash())
            throw std::runtime_error(std::string(what)+": diverged "+std::to_string(t+1)+" ticks after the copy");
    }
}
// Orders that keep routes in play: fields, chains, shared goals and regrouping.
void shuffleOrders(World& world,const std::vector<int>& ids,std::mt19937& rng,int cells) {
    const int groups=4;
    for(int g=0;g<groups;++g) {
        const float x=float(16+rng()%uint32_t(cells-32))*16,z=float(16+rng()%uint32_t(cells-32))*16;
        for(size_t i=size_t(g);i<ids.size();i+=size_t(groups))world.order(ids[i],x,z,false);
    }
}
struct Army {
    UnitType type=mover();
    std::vector<int> ids;
    std::mt19937 rng{1234};
};
void build(World& world,Army& army,PathfindingMode mode,int cells,int rows,bool walls) {
    world.setGameSeed(7);
    setup(world,mode,true,false,cells);
    world.setPlayerCount(2);world.setTeam(0,0);world.setTeam(1,1);
    if(walls)for(int w=1;w<4;++w) {   // three long walls with a gap each, so routes go round
        const int x=w*cells/4;
        world.blockCells(x,0,2,cells-20,true);
    }
    for(int z=0;z<rows;++z)for(int x=0;x<rows;++x)
        army.ids.push_back(world.spawn(&army.type,float((8+x*3)*16),float((8+z*3)*16),std::nullopt,(x+z)%2));
    known(world);
}

void exactAtEveryStage(PathfindingMode mode,const char* what,int cells,int rows,bool wantChains=false) {
    World world;Army army;build(world,army,mode,cells,rows,true);
    world.tick(1.f/30);
    int copies=0;
    bool chains=false;
    for(int round=0;round<14;++round) {
        if(round%3==0)shuffleOrders(world,army.ids,army.rng,cells);
        for(int t=0;t<23;++t)world.tick(1.f/30);
        if(mode==PathfindingMode::Legion)chains|=world.legionNavigator()->accounting().bytes.tiles>0;
        auto copy=world.clone();
        require(copy->stateHash()==world.stateHash(),"a copy hashes differently from its original");
        lockstep(world,*copy,40,what);
        ++copies;
    }
    require(!wantChains||chains,"the fixture never held a tile chain, so it proves nothing about them");
    std::printf("  %s: %d copies stayed in step\n",what,copies);
}

void separate(PathfindingMode mode) {
    World world;Army army;build(world,army,mode,128,4,false);
    shuffleOrders(world,army.ids,army.rng,128);
    for(int t=0;t<60;++t)world.tick(1.f/30);
    // A world that never saw what is about to happen to the original.
    World twin;Army twinArmy;build(twin,twinArmy,mode,128,4,false);
    shuffleOrders(twin,twinArmy.ids,twinArmy.rng,128);
    for(int t=0;t<60;++t)twin.tick(1.f/30);
    auto copy=world.clone();
    const uint64_t kept=copy->stateHash();
    // The original goes its own way, hard: orders, a death, a walled map, ticks.
    shuffleOrders(world,army.ids,army.rng,128);
    world.unit(army.ids.front())->hp=Fixed();
    world.blockCells(20,20,88,88,true);
    require(!world.navFor(&army.type).walkable(40,40),"the wall went up");
    require(copy->navFor(&army.type).walkable(40,40),"a copy's grid reads its original's obstacles");
    UnitType keep=mover();
    keep.canMove=false;keep.maxVel=Fixed();keep.footX=keep.footZ=24;   // stamps the shared obstacle overlay
    require(world.spawn(&keep,float(32*16),float(32*16),std::nullopt,0)>0,"the fixture's building was refused");
    for(int t=0;t<120;++t)world.tick(1.f/30);
    require(copy->stateHash()==kept,"the original reached into its copy");
    const uint64_t farAhead=world.stateHash();
    // Routes asked for now read the maps: the copy's own, as in the twin, not the original's.
    std::mt19937 copyOrders{5},twinOrders{5};
    shuffleOrders(*copy,army.ids,copyOrders,128);
    shuffleOrders(twin,twinArmy.ids,twinOrders,128);
    lockstep(*copy,twin,150,"a copy after its original changed");
    require(world.stateHash()==farAhead,"the copy reached into its original");
    // And the original is unhurt: it still runs in step with a copy of itself.
    auto again=world.clone();
    lockstep(world,*again,60,"after the copies separated");
}

// A copy of a copy, its source then freed and the memory reused: nothing of the source
// may still be read. (World holds no such pointer; a Legion route's tiles did, once.)
void outlivesItsSource() {
    World world;Army army;build(world,army,PathfindingMode::Legion,1024,5,true);
    world.tick(1.f/30);
    bool chains=false;
    for(int round=0;round<8;++round) {
        if(round%3==0)shuffleOrders(world,army.ids,army.rng,1024);
        for(int t=0;t<23;++t)world.tick(1.f/30);
        chains|=world.legionNavigator()->accounting().bytes.tiles>0;
        auto source=world.clone();
        auto copy=source->clone();
        source.reset();
        std::vector<std::vector<uint32_t>> rubble;
        for(size_t size:{size_t(1024),size_t(4096),size_t(16384),size_t(65536)})
            for(int i=0;i<256;++i)rubble.emplace_back(size/sizeof(uint32_t),0xABABABABu);
        lockstep(world,*copy,30,"after its source was freed");
    }
    require(chains,"the fixture never held a tile chain");
}

void restoreInPlace(PathfindingMode mode) {
    World world;Army army;build(world,army,mode,128,4,true);
    shuffleOrders(world,army.ids,army.rng,128);
    for(int t=0;t<50;++t)world.tick(1.f/30);
    auto keyframe=world.clone();
    const uint64_t at=world.stateHash();
    std::mt19937 rng{99};
    shuffleOrders(world,army.ids,rng,128);
    std::vector<uint64_t> straight;
    for(int t=0;t<150;++t) {world.tick(1.f/30);straight.push_back(world.stateHash());}
    // Twice from the one keyframe: a restore must leave it reusable.
    for(int pass=0;pass<2;++pass) {
        const World* address=&world;
        world.restoreFrom(*keyframe);
        require(address==&world&&world.stateHash()==at,"a restore did not land on the keyframe's state");
        rng.seed(99);
        shuffleOrders(world,army.ids,rng,128);
        for(int t=0;t<150;++t) {
            world.tick(1.f/30);
            if(world.stateHash()!=straight[size_t(t)])
                throw std::runtime_error("restored run diverged from the straight run at +"+std::to_string(t));
        }
    }
    require(keyframe->stateHash()==at,"a restore changed its keyframe");
}

// A copy shares its original's per-cell maps (Legion's planes, the grids' clearances) for
// as long as neither changes one. A structure that goes up in ONE of the two must show in
// that one alone: from there each runs, hash for hash on every tick, as a world that was
// never copied and saw the same thing does.
void sharedMapsPart(bool changeTheCopy) {
    const int cells=256;
    UnitType keep=mover();
    keep.canMove=false;keep.maxVel=Fixed();keep.footX=keep.footZ=12;
    Army army,armyWith,armyWithout;
    World world,withIt,without;   // the two references are never copied
    build(world,army,PathfindingMode::Legion,cells,6,true);
    build(withIt,armyWith,PathfindingMode::Legion,cells,6,true);
    build(without,armyWithout,PathfindingMode::Legion,cells,6,true);
    for(World* w:{&world,&withIt,&without}) {
        std::mt19937 rng{31};
        shuffleOrders(*w,army.ids,rng,cells);
        for(int t=0;t<45;++t)w->tick(1.f/30);
    }
    require(world.stateHash()==withIt.stateHash()&&world.stateHash()==without.stateHash(),"the fixture's three worlds differ");
    auto copy=world.clone();
    const uint64_t shared=world.copyOnWriteBytes();
    require(shared>0&&copy->copyOnWriteBytes()==shared,"the fixture's copy shares no map with its original");
    World& changed=changeTheCopy?*copy:world;
    World& same=changeTheCopy?world:*copy;
    // Units are under way in all four; the structure lands in their path in two of them.
    const float at=float(100*16);
    require(changed.spawn(&keep,at,at,std::nullopt,0)>0&&withIt.spawn(&keep,at,at,std::nullopt,0)>0,
            "the fixture's building was refused");
    std::mt19937 orders[4]={std::mt19937{77},std::mt19937{77},std::mt19937{77},std::mt19937{77}};
    World* const all[4]={&changed,&withIt,&same,&without};
    for(int t=0;t<240;++t) {
        if(t%60==20)for(int i=0;i<4;++i)shuffleOrders(*all[i],army.ids,orders[i],cells);
        for(World* w:all)w->tick(1.f/30);
        if(changed.stateHash()!=withIt.stateHash())
            throw std::runtime_error(std::string(changeTheCopy?"a copy":"an original")+
                " that changed a shared map parted from a world never copied, "+std::to_string(t+1)+" ticks on");
        if(same.stateHash()!=without.stateHash())
            throw std::runtime_error(std::string(changeTheCopy?"an original":"a copy")+
                " saw the other's structure through a shared map, "+std::to_string(t+1)+" ticks on");
    }
    require(changed.copyOnWriteBytes()<shared,"the structure changed none of the shared maps, so the fixture proves nothing");
    // And each world's plane is its own: the structure's ground is closed in one and open in the other, and
    // both planes equal a rebuild from their own world.
    const Unit* inChanged=changed.unit(army.ids.front());
    const Unit* inSame=same.unit(army.ids.front());
    require(inChanged&&inSame,"the fixture lost its first unit");
    require(!changed.legionNavigator()->staticLegal(*inChanged,100,100)&&same.legionNavigator()->staticLegal(*inSame,100,100),
            "the structure stands in the wrong world's plane");
    require(changed.legionNavigator()->planeMatchesRebuild(*inChanged)&&same.legionNavigator()->planeMatchesRebuild(*inSame),
            "a plane differs from a rebuild out of its own world");
}
void sharedMaps() {
    sharedMapsPart(false);
    sharedMapsPart(true);
    std::puts("  shared maps: a structure in the original, then in the copy, stayed in its own world");
}

// A copy takes neither what it can share with its original (the terrain's grids, Legion's
// planes) nor what is rebuilt on demand (component labels, the planes' memos, scratch).
// A replay keyframe is such a copy: they were 1.1 GB each on a 2048x2048 map, which left
// room for three. Bytes per map cell, so the figure reads the same on any map size.
void copyIsSmall() {
    const int cells=1024;
    World world;Army army;build(world,army,PathfindingMode::Legion,cells,5,true);
    world.tick(1.f/30);
    for(int round=0;round<3;++round) {
        shuffleOrders(world,army.ids,army.rng,cells);
        for(int t=0;t<40;++t)world.tick(1.f/30);
    }
    // The component labels the AI and the builders ask for (a pure memo, megabytes a grid).
    (void)world.pathExists(&army.type,float(900*16),float(900*16),float(10*16),float(10*16));
    uint64_t bytes=0,keptBytes=0;
    {
        ta::client::AllocCount counted;
        const auto copy=world.clone();
        bytes=counted.bytes();
        require(copy->stateHash()==world.stateHash(),"a copy hashes differently from its original");
    }
    {
        ta::client::AllocCount counted;
        const auto kept=world.keyframe();
        keptBytes=counted.bytes();
    }
    const double perCell=double(bytes)/double(cells)/double(cells),keptPerCell=double(keptBytes)/double(cells)/double(cells);
    std::printf("  a %dx%d Legion world: a copy that runs took %.1f bytes a cell (%.1f MB), a keyframe %.1f (%.1f MB)\n",
                cells,cells,perCell,double(bytes)/1048576.0,keptPerCell,double(keptBytes)/1048576.0);
    require(perCell<kCopyBytesPerCell,"a copy of the world takes what it could share or rebuild");
    require(keptPerCell<kKeyframeBytesPerCell,"a keyframe of the world holds its sparse maps as arrays");
}

// PackedWhenKept: whatever the array holds, a copy made to keep gives it back exactly, and
// a copy made to run is the array itself.
template<class T,class Fill> void packedPart(size_t n,const char* what,bool shouldPack,Fill fill) {
    PackedWhenKept<T> array;
    array.resize(n);
    fill(array);
    const std::vector<T> plain=array;
    const PackedWhenKept<T> running=array;
    require(!running.packed()&&static_cast<const std::vector<T>&>(running)==plain,"a copy made to run is not the array");
    std::unique_ptr<PackedWhenKept<T>> kept;
    {
        const KeepingCopies keeping;
        kept=std::make_unique<PackedWhenKept<T>>(array);
        const PackedWhenKept<T> again=*kept;   // a kept copy of a kept copy
        require(again.packed()==kept->packed(),"a kept copy of a kept copy changed form");
    }
    if(kept->packed()!=shouldPack)throw std::runtime_error(std::string(what)+(shouldPack?": did not pack":": packed, at a loss"));
    require(!kept->packed()||kept->empty(),"a packed copy still holds the array");
    const PackedWhenKept<T> back=*kept;   // copied from: the array again
    require(!back.packed()&&static_cast<const std::vector<T>&>(back)==plain,"an array copied from its kept copy differs");
    PackedWhenKept<T> assigned;
    assigned.resize(n/2+3);   // over whatever the destination held
    assigned=*kept;
    require(!assigned.packed()&&static_cast<const std::vector<T>&>(assigned)==plain,"an array assigned from its kept copy differs");
    PackedWhenKept<T> moved=std::move(assigned);
    require(static_cast<const std::vector<T>&>(moved)==plain,"a moved array differs");
}
// SharedPackedArray: shared by a copy made to run, runs of its own in a copy made to keep.
template<class Fill> void sharedPackedPart(size_t n,const char* what,bool shouldPack,Fill fill) {
    std::vector<int32_t> plain(n);
    fill(plain);
    SharedPackedArray<int32_t> array;
    array=std::vector<int32_t>(plain);
    const SharedPackedArray<int32_t> running=array;
    require(!running.packed()&&running.data()==array.data()&&(n==0||array.shared()),"a copy made to run does not share the array");
    std::unique_ptr<SharedPackedArray<int32_t>> kept;
    {
        const KeepingCopies keeping;
        kept=std::make_unique<SharedPackedArray<int32_t>>(array);
        const SharedPackedArray<int32_t> again=*kept;
        require(again.packed()==kept->packed(),"a kept copy of a kept copy changed form");
    }
    if(kept->packed()!=shouldPack)throw std::runtime_error(std::string(what)+(shouldPack?": did not pack":": packed, at a loss"));
    require(kept->packed()?kept->empty()&&kept->sharedBytes()==0:kept->data()==array.data(),
            "a kept copy neither packed nor shared the array");
    SharedPackedArray<int32_t> back=*kept;
    require(!back.packed()&&back.size()==n&&std::equal(back.begin(),back.end(),plain.begin()),"an array copied from its kept copy differs");
    SharedPackedArray<int32_t> assigned;
    assigned.assign(n/2+3,7);   // over whatever the destination held
    assigned=*kept;
    require(assigned.size()==n&&std::equal(assigned.begin(),assigned.end(),plain.begin()),"an array assigned from its kept copy differs");
    if(n) {   // and what came back is its holder's own: a write there reaches neither the original nor the kept copy
        back.edit()[n/2]=plain[n/2]+1;
        require(array[n/2]==plain[n/2],"a write to a restored array reached the original");
        const SharedPackedArray<int32_t> fresh=*kept;
        require(fresh[n/2]==plain[n/2],"a write to a restored array reached its kept copy");
    }
}
void packedArrays() {
    std::mt19937 rng{2026};
    struct Cell {   // a placement cell's shape
        uint16_t feature;uint8_t low,high,backX,backZ,owner,spare;
        bool operator==(const Cell&) const=default;
    };
    for(const size_t n:{size_t(0),size_t(1),size_t(7),size_t(4095),size_t(4096),size_t(65536),size_t(1)<<20}) {
        const bool big=n>=4096;
        packedPart<int32_t>(n,"zeros",big,[](auto& a) {std::fill(a.begin(),a.end(),0);});
        packedPart<int32_t>(n,"zeros with bodies",big,[&](auto& a) {
            std::fill(a.begin(),a.end(),0);
            for(size_t k=0;k<n/200;++k) {const size_t at=rng()%n;for(size_t i=at;i<std::min(n,at+4);++i)a[i]=int32_t(1+rng()%5000);}
        });
        packedPart<uint16_t>(n,"sight counts",big,[&](auto& a) {
            uint16_t value=0;
            for(size_t i=0;i<n;++i) {if(rng()%97==0)value=uint16_t(rng()%9);a[i]=value;}
        });
        packedPart<uint8_t>(n,"noise",false,[&](auto& a) {for(auto& v:a)v=uint8_t(rng());});
        packedPart<uint32_t>(n,"a ramp",false,[](auto& a) {for(size_t i=0;i<a.size();++i)a[i]=uint32_t(i);});
        packedPart<uint8_t>(n,"runs between noise",big,[&](auto& a) {
            for(size_t i=0;i<n;++i)a[i]=i%1000<120?uint8_t(rng()):uint8_t(i/1000);
        });
        packedPart<Cell>(n,"placement cells",big,[&](auto& a) {
            for(size_t i=0;i<n;++i)a[i]=Cell{uint16_t(i%511==0?7:0xffff),uint8_t(40+i/5000%3),uint8_t(41),0,0,uint8_t(255),0};
        });
        packedPart<int32_t>(n,"one long run after a literal",big,[](auto& a) {for(size_t i=0;i<a.size();++i)a[i]=i<5?int32_t(i+1):-1;});
        packedPart<int32_t>(n,"a short tail",big,[](auto& a) {std::fill(a.begin(),a.end(),3);if(a.size()>2){a[a.size()-1]=9;a[a.size()-2]=8;}});
        sharedPackedPart(n,"component labels",big,[](auto& a) {for(size_t i=0;i<a.size();++i)a[i]=i%700<40?-1:int32_t(i/9000);});
        sharedPackedPart(n,"shared noise",false,[&](auto& a) {for(auto& v:a)v=int32_t(rng());});
    }
    std::puts("  packed arrays: every shape came back exactly");
}

// A keyframe is a copy to restore from: smaller than a copy that runs, exact when it is
// restored, and still what it was after any number of restores.
void keyframes(PathfindingMode mode,const char* what) {
    World world;Army army;build(world,army,mode,256,6,true);
    shuffleOrders(world,army.ids,army.rng,256);
    for(int t=0;t<70;++t)world.tick(1.f/30);
    const uint64_t at=world.stateHash();
    uint64_t runBytes=0,keptBytes=0;
    std::unique_ptr<World> kept;
    {ta::client::AllocCount counted;const auto copy=world.clone();runBytes=counted.bytes();}
    {ta::client::AllocCount counted;kept=world.keyframe();keptBytes=counted.bytes();}
    require(kept->isKeyframe()&&!world.isKeyframe(),"a keyframe is not marked as one");
    require(keptBytes*4<runBytes*3,"a keyframe is no smaller than a copy that runs");
    bool refused=false;
    try {kept->tick(1.f/30);} catch(const std::logic_error&) {refused=true;}
    require(refused,"a keyframe let itself be run");
    refused=false;
    try {(void)kept->stateHash();} catch(const std::logic_error&) {refused=true;}
    require(refused,"a keyframe let itself be hashed");
    std::mt19937 rng{99};
    shuffleOrders(world,army.ids,rng,256);
    std::vector<uint64_t> straight;
    for(int t=0;t<150;++t) {world.tick(1.f/30);straight.push_back(world.stateHash());}
    // From the keyframe twice, and from a keyframe of a keyframe: each restore runs as the straight run did.
    const auto second=kept->keyframe();
    for(const World* from:{kept.get(),kept.get(),second.get()}) {
        world.restoreFrom(*from);
        require(!world.isKeyframe()&&world.stateHash()==at,"a restore did not land on the keyframe's state");
        rng.seed(99);
        shuffleOrders(world,army.ids,rng,256);
        for(int t=0;t<150;++t) {
            world.tick(1.f/30);
            if(world.stateHash()!=straight[size_t(t)])
                throw std::runtime_error(std::string(what)+": a run restored from a keyframe diverged at +"+std::to_string(t));
        }
    }
    // A copy of a keyframe made to run is a world again.
    const auto awake=kept->clone();
    require(!awake->isKeyframe()&&awake->stateHash()==at,"a running copy of a keyframe is not the world it kept");
    rng.seed(99);
    shuffleOrders(*awake,army.ids,rng,256);
    for(int t=0;t<150;++t) {
        awake->tick(1.f/30);
        if(awake->stateHash()!=straight[size_t(t)])
            throw std::runtime_error(std::string(what)+": a running copy of a keyframe diverged at +"+std::to_string(t));
    }
    std::printf("  %s keyframe: %.1f MB against %.1f MB for a copy that runs, exact through three restores\n",what,
                double(keptBytes)/1048576.0,double(runBytes)/1048576.0);
}

void poolAndObserversStay() {
    World world;Army army;
    build(world,army,PathfindingMode::Retail,64,2,false);
    int observed=0;
    world.setRngObserver([&](const World::RngObservation&){++observed;});
    for(int t=0;t<3;++t)world.tick(1.f/30);
    require(observed>0,"the fixture draws no random numbers, so it proves nothing about observers");
    auto copy=world.clone();
    observed=0;
    for(int t=0;t<30;++t)copy->tick(1.f/30);
    require(observed==0,"a copy carried its original's observer");
    world.setRngObserver(nullptr);
    for(int t=0;t<30;++t)world.tick(1.f/30);
    copy->setSerialThreads(false);world.setSerialThreads(false);
    lockstep(world,*copy,30,"each on a thread pool of its own");
}

void missionData(const std::string& data) {
    auto vfs=ta::hpi::mountPlayVfs(data,{},{},true,nullptr);
    TypeRegistry registry;setupRegistry(registry,vfs,false);
    const char* stem="takmission22_ph";
    World world,other;int human=0,otherHuman=0;
    require(setupMission(world,registry,vfs,stem,human)&&setupMission(other,registry,vfs,stem,otherHuman),
            "no campaign mission in the test data");
    require(world.mission()&&world.mission()->ok(),"this mission has no script to copy");
    world.setSerialThreads(true);other.setSerialThreads(true);
    lockstep(world,other,300,"two loads of the mission");
    auto copy=world.clone();
    for(int t=0;t<600;++t)other.tick(1.f/30);
    lockstep(world,*copy,600,"campaign mission");
    require(copy->stateHash()==other.stateHash(),"a copy parted from a straight run of the mission");
    // With real types the walkers read the per-class grids: wall off the original and send
    // everyone across the map. The copy must walk as a world that never saw the wall does.
    world.blockCells(0,0,1<<14,1<<14,true);
    float goalX=0,goalZ=0;
    for(const auto& u:copy->units())if(u.alive()&&u.type&&u.player==human&&u.type->canMove) {
        goalX=u.x.toFloat()+900;goalZ=u.z.toFloat()+500;
        const int cx=int(u.x.toFloat())/16,cz=int(u.z.toFloat())/16;
        require(!world.navFor(u.type).walkable(cx,cz),"the wall went up");
        require(copy->navFor(u.type).walkable(cx,cz)==other.navFor(u.type).walkable(cx,cz),
                "a copy's grids read its original's obstacles");
        break;
    }
    for(World* w:{copy.get(),&other})
        for(const auto& u:w->units())
            if(u.alive()&&u.type&&u.type->canMove&&u.player==human)w->order(u.id,goalX,goalZ,false);
    lockstep(*copy,other,300,"campaign mission, after its original was walled off");
    std::printf("  mission %s stayed in step\n",stem);
}
}

int main(int argc,char** argv) {try {
    exactAtEveryStage(PathfindingMode::Retail,"retail",128,5);
    exactAtEveryStage(PathfindingMode::Legion,"legion",256,8);
    exactAtEveryStage(PathfindingMode::Legion,"legion over tile chains",1024,5,true);
    separate(PathfindingMode::Retail);
    separate(PathfindingMode::Legion);
    outlivesItsSource();
    restoreInPlace(PathfindingMode::Retail);
    restoreInPlace(PathfindingMode::Legion);
    sharedMaps();
    copyIsSmall();
    packedArrays();
    keyframes(PathfindingMode::Retail,"retail");
    keyframes(PathfindingMode::Legion,"legion");
    poolAndObserversStay();
    if(argc>2&&std::string(argv[1])=="--data")missionData(argv[2]);
    std::puts("world clone passed");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"world clone: %s\n",error.what());return 1;}}
